//===- IncrementalPDBCache.cpp -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "IncrementalPDBCache.h"
#include "COFFLinkerContext.h"
#include "Config.h"
#include "DebugTypes.h"
#include "Incremental.h"
#include "IncrementalPDBCacheFormat.h"
#include "InputFiles.h"
#include "lld/Common/Timer.h"
#include "lld/Common/Version.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <cstring>

using namespace llvm;
using namespace llvm::codeview;

namespace lld::coff {

namespace {

struct StringTableBuilder {
  uint64_t add(StringRef value) {
    auto [it, inserted] = offsets.try_emplace(value.str(), data.size());
    if (!inserted)
      return it->second;
    data.append(value.begin(), value.end());
    data.push_back('\0');
    return it->second;
  }

  StringMap<uint64_t> offsets;
  std::string data;
};

template <typename T>
Expected<T> readObject(ArrayRef<uint8_t> bytes, uint64_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache is truncated");
  T object;
  memcpy(&object, bytes.data() + offset, sizeof(T));
  return object;
}

template <typename T>
Expected<ArrayRef<T>> readTable(ArrayRef<uint8_t> bytes, uint64_t offset,
                                uint32_t count) {
  if (count == 0)
    return ArrayRef<T>();
  if (offset > bytes.size())
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache table offset is out of range");
  uint64_t tableBytes = uint64_t(count) * sizeof(T);
  if (tableBytes > bytes.size() || bytes.size() - offset < tableBytes)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache table is truncated");
  return ArrayRef<T>(reinterpret_cast<const T *>(bytes.data() + offset), count);
}

Expected<StringRef> loadString(ArrayRef<uint8_t> strings, uint64_t offset) {
  if (offset > strings.size())
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache string offset is out of range");
  const char *data = reinterpret_cast<const char *>(strings.data() + offset);
  size_t maxLength = strings.size() - offset;
  size_t length = strnlen(data, maxLength);
  if (length == maxLength)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache string is unterminated");
  return StringRef(data, length);
}

Expected<ArrayRef<uint8_t>> loadBlob(ArrayRef<uint8_t> bytes, uint64_t offset,
                                     uint64_t size) {
  if (size == 0)
    return ArrayRef<uint8_t>();
  if (offset > bytes.size() || size > bytes.size() || bytes.size() - offset < size)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache blob is truncated");
  return bytes.slice(offset, size);
}

template <typename T>
void appendObject(std::vector<char> &buffer, const T &value) {
  size_t oldSize = buffer.size();
  buffer.resize(oldSize + sizeof(T));
  memcpy(buffer.data() + oldSize, &value, sizeof(T));
}

template <typename T>
IncrementalPDBBlobRefRecord appendBlob(std::vector<char> &blobArena,
                                       ArrayRef<T> values) {
  IncrementalPDBBlobRefRecord ref = {};
  if (values.empty())
    return ref;
  ref.offset = blobArena.size();
  ref.size = values.size() * sizeof(T);
  const char *data = reinterpret_cast<const char *>(values.data());
  blobArena.insert(blobArena.end(), data, data + ref.size);
  return ref;
}

static std::string getCompositeKey(StringRef path, StringRef parentPath,
                                   uint64_t archiveOffset) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  os << path << '\n' << parentPath << '\n' << archiveOffset;
  return std::string(buffer);
}

static std::string getTypeCompositeKey(const IncrementalPDBTypeCacheEntry &entry) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  os << entry.path << '\n' << entry.parentPath << '\n' << entry.archiveOffset
     << '\n' << unsigned(entry.kind);
  return std::string(buffer);
}

static std::string
getModuleCompositeKey(const IncrementalPDBModuleCacheEntry &entry) {
  return getCompositeKey(entry.path, entry.parentPath, entry.archiveOffset);
}

static void hashChunkContents(raw_ostream &os, ArrayRef<SectionChunk *> chunks,
                              StringRef sectionNameFilter) {
  for (SectionChunk *chunk : chunks) {
    if (!chunk->live || chunk->getSize() == 0 ||
        chunk->getSectionName() != sectionNameFilter)
      continue;
    ArrayRef<uint8_t> contents = chunk->getContents();
    os << chunk->getSectionName() << '\n' << contents.size() << '\n';
    os.write(reinterpret_cast<const char *>(contents.data()), contents.size());
  }
}

static bool hasBitcodeInputs(const COFFLinkerContext &ctx) {
  bool result = false;
  const_cast<COFFLinkerContext &>(ctx).forEachSymtab([&](SymbolTable &symtab) {
    result |= !symtab.bitcodeFileInstances.empty();
  });
  return result;
}

} // namespace

SmallString<128> getIncrementalPDBCachePath(const Configuration &config) {
  SmallString<128> path(config.outputFile);
  if (!path.empty())
    sys::path::replace_extension(path, ".llpdbcache");
  return path;
}

IncrementalPDBCacheRuntimeMode
classifyIncrementalPDBCacheRuntimeMode(const COFFLinkerContext &ctx) {
  if (ctx.config.machine != AMD64 || !ctx.config.debug ||
      ctx.config.pdbPath.empty() || hasBitcodeInputs(ctx))
    return IncrementalPDBCacheRuntimeMode::BypassCache;

  bool mayReplay = findActiveByteReuseLink(ctx) != nullptr;
  bool mayRecord = shouldEmitIncrementalBaseline(ctx);
  if (mayReplay && mayRecord)
    return IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache;
  if (mayReplay)
    return IncrementalPDBCacheRuntimeMode::ReplayOnlyCache;
  if (mayRecord)
    return IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
  return IncrementalPDBCacheRuntimeMode::BypassCache;
}

uint64_t computeIncrementalPDBCacheBuildId() {
  return xxh3_64bits(getLLDVersion());
}

uint64_t computeIncrementalPDBCacheHardConfigHash(const Configuration &config) {
  SmallString<64> buffer;
  raw_svector_ostream os(buffer);
  os << uint32_t(config.machine) << '\n' << "phase4a";
  return xxh3_64bits(buffer);
}

std::string getIncrementalPDBCacheObjectKey(const ObjFile &file) {
  return getCompositeKey(file.getName(), file.archiveName, file.archiveOffset);
}

uint64_t computeIncrementalPDBModuleDebugSHash(const ObjFile &file) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  hashChunkContents(os, file.getDebugChunks(), ".debug$S");
  return xxh3_64bits(ArrayRef(
      reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size()));
}

uint64_t computeIncrementalPDBModuleDebugFHash(const ObjFile &file) {
  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  hashChunkContents(os, file.getDebugChunks(), ".debug$F");
  return xxh3_64bits(ArrayRef(
      reinterpret_cast<const uint8_t *>(buffer.data()), buffer.size()));
}

uint64_t computeIncrementalPDBModuleRelocHash(const ObjFile &file) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  for (SectionChunk *chunk : file.getDebugChunks()) {
    if (!chunk->live || chunk->getSize() == 0)
      continue;
    StringRef name = chunk->getSectionName();
    if (name != ".debug$S" && name != ".debug$F")
      continue;
    chunk->sortRelocations();
    os << name << '\n';
    for (const object::coff_relocation &rel : chunk->getRelocs())
      os << rel.VirtualAddress << '\n' << rel.SymbolTableIndex << '\n'
         << rel.Type << '\n';
  }
  return xxh3_64bits(buffer);
}

Expected<IncrementalPDBCacheFile> loadIncrementalPDBCache(StringRef path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!buffer)
    return createFileError(path, errorCodeToError(buffer.getError()));

  ArrayRef<uint8_t> bytes(
      reinterpret_cast<const uint8_t *>((*buffer)->getBufferStart()),
      (*buffer)->getBufferSize());
  Expected<IncrementalPDBCacheHeader> headerOrErr =
      readObject<IncrementalPDBCacheHeader>(bytes, 0);
  if (!headerOrErr)
    return headerOrErr.takeError();
  const IncrementalPDBCacheHeader &header = *headerOrErr;

  if (memcmp(header.magic, incrementalPDBCacheMagic,
             sizeof(incrementalPDBCacheMagic)) != 0)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache has an invalid magic");
  if (header.version != incrementalPDBCacheVersion)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache has an unsupported version");

  if (header.stringTableOffset > bytes.size() ||
      header.stringTableSize > bytes.size() ||
      bytes.size() - header.stringTableOffset < header.stringTableSize)
    return createStringError(inconvertibleErrorCode(),
                             "incremental PDB cache string table is truncated");
  ArrayRef<uint8_t> strings =
      bytes.slice(header.stringTableOffset, header.stringTableSize);
  Expected<ArrayRef<uint8_t>> blobBytesOrErr =
      loadBlob(bytes, header.blobOffset, header.blobSize);
  if (!blobBytesOrErr)
    return blobBytesOrErr.takeError();
  ArrayRef<uint8_t> blobBytes = *blobBytesOrErr;

  auto typeRecordsOrErr = readTable<IncrementalPDBTypeEntryRecord>(
      bytes, header.typeTableOffset, header.typeCount);
  if (!typeRecordsOrErr)
    return typeRecordsOrErr.takeError();
  auto moduleRecordsOrErr = readTable<IncrementalPDBModuleEntryRecord>(
      bytes, header.moduleTableOffset, header.moduleCount);
  if (!moduleRecordsOrErr)
    return moduleRecordsOrErr.takeError();
  auto chunkRecordsOrErr = readTable<IncrementalPDBChunkPlanRecord>(
      bytes, header.chunkPlanTableOffset, header.chunkPlanCount);
  if (!chunkRecordsOrErr)
    return chunkRecordsOrErr.takeError();
  auto subsectionRecordsOrErr = readTable<IncrementalPDBSubsectionPlanRecord>(
      bytes, header.subsectionPlanTableOffset, header.subsectionPlanCount);
  if (!subsectionRecordsOrErr)
    return subsectionRecordsOrErr.takeError();
  auto symbolRecordsOrErr = readTable<IncrementalPDBSymbolPlanRecord>(
      bytes, header.symbolPlanTableOffset, header.symbolPlanCount);
  if (!symbolRecordsOrErr)
    return symbolRecordsOrErr.takeError();
  auto typeRefRecordsOrErr = readTable<IncrementalPDBTypeRefRecord>(
      bytes, header.typeRefTableOffset, header.typeRefCount);
  if (!typeRefRecordsOrErr)
    return typeRefRecordsOrErr.takeError();
  auto stringFixupRecordsOrErr = readTable<IncrementalPDBStringFixupRecord>(
      bytes, header.stringFixupTableOffset, header.stringFixupCount);
  if (!stringFixupRecordsOrErr)
    return stringFixupRecordsOrErr.takeError();

  IncrementalPDBCacheFile cache;
  cache.linkerBuildId = header.linkerBuildId;
  cache.hardConfigHash = header.hardConfigHash;

  cache.typeEntries.reserve(typeRecordsOrErr->size());
  for (const IncrementalPDBTypeEntryRecord &record : *typeRecordsOrErr) {
    IncrementalPDBTypeCacheEntry entry;
    auto pathOrErr = loadString(strings, record.pathOffset);
    if (!pathOrErr)
      return pathOrErr.takeError();
    auto parentOrErr = loadString(strings, record.parentPathOffset);
    if (!parentOrErr)
      return parentOrErr.takeError();
    auto ghashBlobOrErr =
        loadBlob(blobBytes, record.ghashes.offset, record.ghashes.size);
    if (!ghashBlobOrErr)
      return ghashBlobOrErr.takeError();
    auto isItemBlobOrErr = loadBlob(blobBytes, record.isItemIndexBits.offset,
                                    record.isItemIndexBits.size);
    if (!isItemBlobOrErr)
      return isItemBlobOrErr.takeError();
    auto auxGHashBlobOrErr =
        loadBlob(blobBytes, record.auxGHashes.offset, record.auxGHashes.size);
    if (!auxGHashBlobOrErr)
      return auxGHashBlobOrErr.takeError();
    auto auxIsItemBlobOrErr =
        loadBlob(blobBytes, record.auxIsItemIndexBits.offset,
                 record.auxIsItemIndexBits.size);
    if (!auxIsItemBlobOrErr)
      return auxIsItemBlobOrErr.takeError();
    if (ghashBlobOrErr->size() % sizeof(GloballyHashedType) != 0 ||
        auxGHashBlobOrErr->size() % sizeof(GloballyHashedType) != 0)
      return createStringError(inconvertibleErrorCode(),
                               "incremental PDB cache ghash blob is misaligned");

    entry.path = pathOrErr->str();
    entry.parentPath = parentOrErr->str();
    entry.archiveOffset = record.archiveOffset;
    entry.kind = static_cast<IncrementalPDBTypeSourceKind>(uint8_t(record.kind));
    entry.contentHash = record.contentHash;
    entry.dependencyHash = record.dependencyHash;
    entry.endPrecompIdx = record.endPrecompIdx;
    entry.ghashes.resize(ghashBlobOrErr->size() / sizeof(GloballyHashedType));
    entry.isItemIndexBits.assign(isItemBlobOrErr->begin(), isItemBlobOrErr->end());
    entry.auxGHashes.resize(auxGHashBlobOrErr->size() / sizeof(GloballyHashedType));
    entry.auxIsItemIndexBits.assign(auxIsItemBlobOrErr->begin(),
                                    auxIsItemBlobOrErr->end());
    memcpy(entry.ghashes.data(), ghashBlobOrErr->data(), ghashBlobOrErr->size());
    memcpy(entry.auxGHashes.data(), auxGHashBlobOrErr->data(),
           auxGHashBlobOrErr->size());
    cache.typeEntries.push_back(std::move(entry));
  }

  cache.moduleEntries.reserve(moduleRecordsOrErr->size());
  for (const IncrementalPDBModuleEntryRecord &record : *moduleRecordsOrErr) {
    if (record.chunkPlanStart > chunkRecordsOrErr->size() ||
        chunkRecordsOrErr->size() - record.chunkPlanStart < record.chunkPlanCount ||
        record.subsectionPlanStart > subsectionRecordsOrErr->size() ||
        subsectionRecordsOrErr->size() - record.subsectionPlanStart <
            record.subsectionPlanCount ||
        record.symbolPlanStart > symbolRecordsOrErr->size() ||
        symbolRecordsOrErr->size() - record.symbolPlanStart <
            record.symbolPlanCount ||
        record.typeRefStart > typeRefRecordsOrErr->size() ||
        typeRefRecordsOrErr->size() - record.typeRefStart < record.typeRefCount ||
        record.stringFixupStart > stringFixupRecordsOrErr->size() ||
        stringFixupRecordsOrErr->size() - record.stringFixupStart <
            record.stringFixupCount)
      return createStringError(inconvertibleErrorCode(),
                               "incremental PDB cache plan range is invalid");

    IncrementalPDBModuleCacheEntry entry;
    auto pathOrErr = loadString(strings, record.pathOffset);
    if (!pathOrErr)
      return pathOrErr.takeError();
    auto parentOrErr = loadString(strings, record.parentPathOffset);
    if (!parentOrErr)
      return parentOrErr.takeError();
    entry.path = pathOrErr->str();
    entry.parentPath = parentOrErr->str();
    entry.archiveOffset = record.archiveOffset;
    entry.debugSHash = record.debugSHash;
    entry.debugFHash = record.debugFHash;
    entry.relocHash = record.relocHash;
    entry.moduleStreamSize = record.moduleStreamSize;

    for (const IncrementalPDBChunkPlanRecord &chunkRecord :
         chunkRecordsOrErr->slice(record.chunkPlanStart, record.chunkPlanCount)) {
      IncrementalPDBChunkPlan plan;
      plan.chunkOrdinal = chunkRecord.chunkOrdinal;
      plan.kind =
          static_cast<IncrementalPDBDebugChunkKind>(uint8_t(chunkRecord.kind));
      plan.subsectionStart = chunkRecord.subsectionStart;
      plan.subsectionCount = chunkRecord.subsectionCount;
      entry.chunkPlans.push_back(plan);
    }

    for (const IncrementalPDBSubsectionPlanRecord &subsectionRecord :
         subsectionRecordsOrErr->slice(record.subsectionPlanStart,
                                       record.subsectionPlanCount)) {
      IncrementalPDBSubsectionPlan plan;
      plan.kind = static_cast<DebugSubsectionKind>(uint16_t(subsectionRecord.kind));
      plan.recordOffset = subsectionRecord.recordOffset;
      plan.recordLength = subsectionRecord.recordLength;
      plan.relocIndex = subsectionRecord.relocIndex;
      plan.symbolPlanStart = subsectionRecord.symbolPlanStart;
      plan.symbolPlanCount = subsectionRecord.symbolPlanCount;
      entry.subsectionPlans.push_back(plan);
    }

    for (const IncrementalPDBSymbolPlanRecord &symbolRecord :
         symbolRecordsOrErr->slice(record.symbolPlanStart, record.symbolPlanCount)) {
      IncrementalPDBSymbolPlan plan;
      plan.recordOffset = symbolRecord.recordOffset;
      plan.recordLength = symbolRecord.recordLength;
      plan.alignedLength = symbolRecord.alignedLength;
      plan.relocIndex = symbolRecord.relocIndex;
      plan.typeRefStart = symbolRecord.typeRefStart;
      plan.typeRefCount = symbolRecord.typeRefCount;
      plan.destMask = symbolRecord.destMask;
      plan.flags = symbolRecord.flags;
      plan.rewriteKind = symbolRecord.rewriteKind;
      plan.scopeAction =
          static_cast<IncrementalPDBScopeAction>(uint8_t(symbolRecord.scopeAction));
      entry.symbolPlans.push_back(plan);
    }

    for (const IncrementalPDBTypeRefRecord &typeRefRecord :
         typeRefRecordsOrErr->slice(record.typeRefStart, record.typeRefCount)) {
      IncrementalPDBTypeRef ref;
      ref.kind = static_cast<TiRefKind>(uint8_t(typeRefRecord.kind));
      ref.offset = typeRefRecord.offset;
      ref.count = typeRefRecord.count;
      entry.typeRefs.push_back(ref);
    }

    for (const IncrementalPDBStringFixupRecord &fixupRecord :
         stringFixupRecordsOrErr->slice(record.stringFixupStart,
                                        record.stringFixupCount)) {
      IncrementalPDBStringFixup fixup;
      fixup.strTabOffset = fixupRecord.strTabOffset;
      fixup.symOffsetOfReference = fixupRecord.symOffsetOfReference;
      entry.stringFixups.push_back(fixup);
    }

    cache.moduleEntries.push_back(std::move(entry));
  }

  return cache;
}

Error writeIncrementalPDBCache(StringRef path,
                               const IncrementalPDBCacheFile &cache) {
  IncrementalPDBCacheHeader header = {};
  memcpy(header.magic, incrementalPDBCacheMagic, sizeof(incrementalPDBCacheMagic));
  header.version = incrementalPDBCacheVersion;
  header.machine = AMD64;
  header.flags = incrementalPDBCacheFlagHasTypeEntries |
                 incrementalPDBCacheFlagHasModuleEntries;
  header.linkerBuildId = cache.linkerBuildId;
  header.hardConfigHash = cache.hardConfigHash;

  StringTableBuilder strings;
  std::vector<IncrementalPDBTypeEntryRecord> typeRecords;
  std::vector<IncrementalPDBModuleEntryRecord> moduleRecords;
  std::vector<IncrementalPDBChunkPlanRecord> chunkRecords;
  std::vector<IncrementalPDBSubsectionPlanRecord> subsectionRecords;
  std::vector<IncrementalPDBSymbolPlanRecord> symbolRecords;
  std::vector<IncrementalPDBTypeRefRecord> typeRefRecords;
  std::vector<IncrementalPDBStringFixupRecord> stringFixupRecords;
  std::vector<char> blobArena;

  typeRecords.reserve(cache.typeEntries.size());
  for (const IncrementalPDBTypeCacheEntry &entry : cache.typeEntries) {
    IncrementalPDBTypeEntryRecord record = {};
    record.pathOffset = strings.add(entry.path);
    record.parentPathOffset = strings.add(entry.parentPath);
    record.archiveOffset = entry.archiveOffset;
    record.kind = uint8_t(entry.kind);
    record.contentHash = entry.contentHash;
    record.dependencyHash = entry.dependencyHash;
    record.endPrecompIdx = entry.endPrecompIdx;
    record.ghashes = appendBlob(blobArena, ArrayRef(entry.ghashes));
    record.isItemIndexBits = appendBlob(blobArena, ArrayRef(entry.isItemIndexBits));
    record.auxGHashes = appendBlob(blobArena, ArrayRef(entry.auxGHashes));
    record.auxIsItemIndexBits =
        appendBlob(blobArena, ArrayRef(entry.auxIsItemIndexBits));
    typeRecords.push_back(record);
  }

  moduleRecords.reserve(cache.moduleEntries.size());
  for (const IncrementalPDBModuleCacheEntry &entry : cache.moduleEntries) {
    IncrementalPDBModuleEntryRecord record = {};
    record.pathOffset = strings.add(entry.path);
    record.parentPathOffset = strings.add(entry.parentPath);
    record.archiveOffset = entry.archiveOffset;
    record.debugSHash = entry.debugSHash;
    record.debugFHash = entry.debugFHash;
    record.relocHash = entry.relocHash;
    record.moduleStreamSize = entry.moduleStreamSize;

    record.chunkPlanStart = chunkRecords.size();
    record.chunkPlanCount = entry.chunkPlans.size();
    for (const IncrementalPDBChunkPlan &plan : entry.chunkPlans) {
      IncrementalPDBChunkPlanRecord chunkRecord = {};
      chunkRecord.chunkOrdinal = plan.chunkOrdinal;
      chunkRecord.kind = uint8_t(plan.kind);
      chunkRecord.subsectionStart = plan.subsectionStart;
      chunkRecord.subsectionCount = plan.subsectionCount;
      chunkRecords.push_back(chunkRecord);
    }

    record.subsectionPlanStart = subsectionRecords.size();
    record.subsectionPlanCount = entry.subsectionPlans.size();
    for (const IncrementalPDBSubsectionPlan &plan : entry.subsectionPlans) {
      IncrementalPDBSubsectionPlanRecord subsectionRecord = {};
      subsectionRecord.kind = uint16_t(plan.kind);
      subsectionRecord.recordOffset = plan.recordOffset;
      subsectionRecord.recordLength = plan.recordLength;
      subsectionRecord.relocIndex = plan.relocIndex;
      subsectionRecord.symbolPlanStart = plan.symbolPlanStart;
      subsectionRecord.symbolPlanCount = plan.symbolPlanCount;
      subsectionRecords.push_back(subsectionRecord);
    }

    record.symbolPlanStart = symbolRecords.size();
    record.symbolPlanCount = entry.symbolPlans.size();
    for (const IncrementalPDBSymbolPlan &plan : entry.symbolPlans) {
      IncrementalPDBSymbolPlanRecord symbolRecord = {};
      symbolRecord.recordOffset = plan.recordOffset;
      symbolRecord.recordLength = plan.recordLength;
      symbolRecord.alignedLength = plan.alignedLength;
      symbolRecord.relocIndex = plan.relocIndex;
      symbolRecord.typeRefStart = plan.typeRefStart;
      symbolRecord.typeRefCount = plan.typeRefCount;
      symbolRecord.destMask = plan.destMask;
      symbolRecord.flags = plan.flags;
      symbolRecord.rewriteKind = plan.rewriteKind;
      symbolRecord.scopeAction = uint8_t(plan.scopeAction);
      symbolRecords.push_back(symbolRecord);
    }

    record.typeRefStart = typeRefRecords.size();
    record.typeRefCount = entry.typeRefs.size();
    for (const IncrementalPDBTypeRef &ref : entry.typeRefs) {
      IncrementalPDBTypeRefRecord typeRefRecord = {};
      typeRefRecord.kind = uint8_t(ref.kind);
      typeRefRecord.offset = ref.offset;
      typeRefRecord.count = ref.count;
      typeRefRecords.push_back(typeRefRecord);
    }

    record.stringFixupStart = stringFixupRecords.size();
    record.stringFixupCount = entry.stringFixups.size();
    for (const IncrementalPDBStringFixup &fixup : entry.stringFixups) {
      IncrementalPDBStringFixupRecord fixupRecord = {};
      fixupRecord.strTabOffset = fixup.strTabOffset;
      fixupRecord.symOffsetOfReference = fixup.symOffsetOfReference;
      stringFixupRecords.push_back(fixupRecord);
    }

    moduleRecords.push_back(record);
  }

  header.stringTableOffset = sizeof(IncrementalPDBCacheHeader);
  header.stringTableSize = strings.data.size();
  header.typeTableOffset = header.stringTableOffset + header.stringTableSize;
  header.typeCount = typeRecords.size();
  header.moduleTableOffset =
      header.typeTableOffset +
      typeRecords.size() * sizeof(IncrementalPDBTypeEntryRecord);
  header.moduleCount = moduleRecords.size();
  header.chunkPlanTableOffset =
      header.moduleTableOffset +
      moduleRecords.size() * sizeof(IncrementalPDBModuleEntryRecord);
  header.chunkPlanCount = chunkRecords.size();
  header.subsectionPlanTableOffset =
      header.chunkPlanTableOffset +
      chunkRecords.size() * sizeof(IncrementalPDBChunkPlanRecord);
  header.subsectionPlanCount = subsectionRecords.size();
  header.symbolPlanTableOffset =
      header.subsectionPlanTableOffset +
      subsectionRecords.size() * sizeof(IncrementalPDBSubsectionPlanRecord);
  header.symbolPlanCount = symbolRecords.size();
  header.typeRefTableOffset =
      header.symbolPlanTableOffset +
      symbolRecords.size() * sizeof(IncrementalPDBSymbolPlanRecord);
  header.typeRefCount = typeRefRecords.size();
  header.stringFixupTableOffset =
      header.typeRefTableOffset +
      typeRefRecords.size() * sizeof(IncrementalPDBTypeRefRecord);
  header.stringFixupCount = stringFixupRecords.size();
  header.blobOffset =
      header.stringFixupTableOffset +
      stringFixupRecords.size() * sizeof(IncrementalPDBStringFixupRecord);
  header.blobSize = blobArena.size();

  std::vector<char> bytes;
  bytes.reserve(header.blobOffset + header.blobSize);
  appendObject(bytes, header);
  bytes.insert(bytes.end(), strings.data.begin(), strings.data.end());
  for (const IncrementalPDBTypeEntryRecord &record : typeRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBModuleEntryRecord &record : moduleRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBChunkPlanRecord &record : chunkRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBSubsectionPlanRecord &record : subsectionRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBSymbolPlanRecord &record : symbolRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBTypeRefRecord &record : typeRefRecords)
    appendObject(bytes, record);
  for (const IncrementalPDBStringFixupRecord &record : stringFixupRecords)
    appendObject(bytes, record);
  bytes.insert(bytes.end(), blobArena.begin(), blobArena.end());

  SmallString<128> tmpPattern(path);
  tmpPattern += ".tmp-%%%%%%%%";
  SmallString<128> tmpName;
  if (std::error_code ec = sys::fs::createUniqueFile(tmpPattern, tmpName))
    return createFileError(path, errorCodeToError(ec));

  std::error_code ec;
  raw_fd_ostream os(tmpName, ec, sys::fs::OF_None);
  if (ec) {
    sys::fs::remove(tmpName);
    return createFileError(tmpName, errorCodeToError(ec));
  }
  os.write(bytes.data(), bytes.size());
  os.close();
  if (os.has_error()) {
    sys::fs::remove(tmpName);
    return createFileError(tmpName, errorCodeToError(os.error()));
  }
  if (std::error_code renameEc = sys::fs::rename(tmpName, path)) {
    sys::fs::remove(tmpName);
    return createFileError(path, errorCodeToError(renameEc));
  }
  return Error::success();
}

IncrementalPDBCacheSession::IncrementalPDBCacheSession(COFFLinkerContext &ctx)
    : ctx(ctx), cachePath(getIncrementalPDBCachePath(ctx.config)),
      mode(classifyIncrementalPDBCacheRuntimeMode(ctx)) {}

std::unique_ptr<IncrementalPDBCacheSession>
IncrementalPDBCacheSession::create(COFFLinkerContext &ctx) {
  auto session = std::unique_ptr<IncrementalPDBCacheSession>(
      new IncrementalPDBCacheSession(ctx));
  switch (session->mode) {
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
    return session;
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  }

  ScopedTimer loadTimer(ctx.pdbCacheLoadTimer);
  Expected<IncrementalPDBCacheFile> cacheOrErr =
      loadIncrementalPDBCache(session->cachePath);
  if (!cacheOrErr) {
    if (ctx.config.verbose)
      Log(ctx) << "pdbcache: ignoring cache '" << session->cachePath
               << "': " << toString(cacheOrErr.takeError());
    if (session->mode == IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache)
      session->mode = IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
    else
      session->mode = IncrementalPDBCacheRuntimeMode::BypassCache;
    return session;
  }

  {
    ScopedTimer validateTimer(ctx.pdbCacheValidateTimer);
    if (cacheOrErr->linkerBuildId != computeIncrementalPDBCacheBuildId() ||
        cacheOrErr->hardConfigHash !=
            computeIncrementalPDBCacheHardConfigHash(ctx.config)) {
      if (ctx.config.verbose)
        Log(ctx) << "pdbcache: cache compatibility mismatch; ignoring '"
                 << session->cachePath << "'";
      if (session->mode == IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache)
        session->mode = IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
      else
        session->mode = IncrementalPDBCacheRuntimeMode::BypassCache;
      return session;
    }
  }

  session->loadedCache = std::move(*cacheOrErr);
  for (const IncrementalPDBTypeCacheEntry &entry : session->loadedCache.typeEntries)
    session->loadedTypesByKey[getTypeCompositeKey(entry)] = &entry;
  for (const IncrementalPDBModuleCacheEntry &entry :
       session->loadedCache.moduleEntries)
    session->loadedModulesByKey[getModuleCompositeKey(entry)] = &entry;
  return session;
}

const IncrementalPDBTypeCacheEntry *
IncrementalPDBCacheSession::findLoadedTypeEntry(StringRef key,
                                                const TpiSource &source) const {
  auto it = loadedTypesByKey.find(key);
  if (it == loadedTypesByKey.end())
    return nullptr;
  const IncrementalPDBTypeCacheEntry *entry = it->second;
  return matchesIncrementalPDBTypeCacheEntry(source, *entry) ? entry : nullptr;
}

const IncrementalPDBModuleCacheEntry *
IncrementalPDBCacheSession::findLoadedModuleEntry(StringRef key,
                                                  const ObjFile &file) const {
  auto it = loadedModulesByKey.find(key);
  if (it == loadedModulesByKey.end())
    return nullptr;
  const IncrementalPDBModuleCacheEntry *entry = it->second;
  if (entry->debugSHash != computeIncrementalPDBModuleDebugSHash(file) ||
      entry->debugFHash != computeIncrementalPDBModuleDebugFHash(file) ||
      entry->relocHash != computeIncrementalPDBModuleRelocHash(file))
    return nullptr;
  return entry;
}

const IncrementalPDBTypeCacheEntry *
IncrementalPDBCacheSession::findTypeEntry(const TpiSource &source) {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
    return nullptr;
  }
  ScopedTimer validateTimer(ctx.pdbCacheValidateTimer);
  std::string key = getIncrementalPDBTypeCacheKey(source);
  if (key.empty())
    return nullptr;
  const IncrementalPDBTypeCacheEntry *entry = findLoadedTypeEntry(key, source);
  if (entry) {
    ++typeCacheHits;
    if (ctx.config.verbose)
      Log(ctx) << "pdbcache: type hit " << key;
    return entry;
  }
  ++typeCacheMisses;
  if (ctx.config.verbose)
    Log(ctx) << "pdbcache: type miss " << key;
  return nullptr;
}

const IncrementalPDBModuleCacheEntry *
IncrementalPDBCacheSession::findModuleEntry(const ObjFile &file) {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
    return nullptr;
  }
  ScopedTimer validateTimer(ctx.pdbCacheValidateTimer);
  const IncrementalPDBModuleCacheEntry *entry =
      findLoadedModuleEntry(getIncrementalPDBCacheObjectKey(file), file);
  if (entry) {
    ++moduleCacheHits;
    if (ctx.config.verbose)
      Log(ctx) << "pdbcache: module hit " << file.getName();
    return entry;
  }
  ++moduleCacheMisses;
  if (ctx.config.verbose)
    Log(ctx) << "pdbcache: module miss " << file.getName();
  return nullptr;
}

void IncrementalPDBCacheSession::recordTypeEntries(ArrayRef<TpiSource *> sources) {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
    return;
  }
  recordedTypeEntries.clear();
  for (TpiSource *source : sources) {
    IncrementalPDBTypeCacheEntry entry;
    if (buildIncrementalPDBTypeCacheEntry(*source, entry))
      recordedTypeEntries.push_back(std::move(entry));
  }
}

Error IncrementalPDBCacheSession::writeCache(
    const DenseMap<const ObjFile *, IncrementalPDBModuleCacheEntry> &modulePlans) const {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
    return Error::success();
  }

  ScopedTimer storeTimer(ctx.pdbCacheStoreTimer);
  IncrementalPDBCacheFile cache;
  cache.linkerBuildId = computeIncrementalPDBCacheBuildId();
  cache.hardConfigHash = computeIncrementalPDBCacheHardConfigHash(ctx.config);
  cache.typeEntries = recordedTypeEntries;
  cache.moduleEntries.reserve(modulePlans.size());
  for (const auto &it : modulePlans)
    cache.moduleEntries.push_back(it.second);
  return writeIncrementalPDBCache(cachePath, cache);
}

} // namespace lld::coff
