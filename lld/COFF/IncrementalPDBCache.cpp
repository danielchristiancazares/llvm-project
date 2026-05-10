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

enum class WireIncrementalPDBTypeReplayKind : uint8_t {
  Object = 1,
  PrecompiledHeader = 2,
  UsingPrecompiledHeader = 3,
  TypeServerTpiOnly = 4,
  TypeServerTpiAndIpi = 5,
};

enum class WireIncrementalPDBChunkReplayKind : uint8_t {
  DebugS = 1,
  DebugF = 2,
};

static Expected<WireIncrementalPDBTypeReplayKind>
decodeWireTypeReplayKind(uint8_t kind) {
  switch (kind) {
  case uint8_t(WireIncrementalPDBTypeReplayKind::Object):
    return WireIncrementalPDBTypeReplayKind::Object;
  case uint8_t(WireIncrementalPDBTypeReplayKind::PrecompiledHeader):
    return WireIncrementalPDBTypeReplayKind::PrecompiledHeader;
  case uint8_t(WireIncrementalPDBTypeReplayKind::UsingPrecompiledHeader):
    return WireIncrementalPDBTypeReplayKind::UsingPrecompiledHeader;
  case uint8_t(WireIncrementalPDBTypeReplayKind::TypeServerTpiOnly):
    return WireIncrementalPDBTypeReplayKind::TypeServerTpiOnly;
  case uint8_t(WireIncrementalPDBTypeReplayKind::TypeServerTpiAndIpi):
    return WireIncrementalPDBTypeReplayKind::TypeServerTpiAndIpi;
  }
  return createStringError(inconvertibleErrorCode(),
                           "incremental PDB cache type replay kind is invalid");
}

static Expected<WireIncrementalPDBChunkReplayKind>
decodeWireChunkReplayKind(uint8_t kind) {
  switch (kind) {
  case uint8_t(WireIncrementalPDBChunkReplayKind::DebugS):
    return WireIncrementalPDBChunkReplayKind::DebugS;
  case uint8_t(WireIncrementalPDBChunkReplayKind::DebugF):
    return WireIncrementalPDBChunkReplayKind::DebugF;
  }
  return createStringError(inconvertibleErrorCode(),
                           "incremental PDB cache chunk replay kind is invalid");
}

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

static IncrementalPDBTypeReplayBoundary
cloneTypeReplayBoundary(const IncrementalPDBTypeReplayBoundary &boundary) {
  return boundary.match(
      [&](const ReplayAllTypeRecords &) {
        return IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
      },
      [&](const ReplayTypeRecordsSkippingEndPrecomp &skip) {
        return IncrementalPDBTypeReplayBoundary::make<
            ReplayTypeRecordsSkippingEndPrecomp>(skip);
      });
}

static uint32_t
encodeWireEndPrecompIdx(const IncrementalPDBTypeReplayBoundary &boundary) {
  return boundary.match(
      [&](const ReplayAllTypeRecords &) { return ~0U; },
      [&](const ReplayTypeRecordsSkippingEndPrecomp &skip) {
        return skip.ghashIndex;
      });
}

static IncrementalPDBTypeReplayBoundary
decodeWireEndPrecompIdx(uint32_t endPrecompIdx) {
  if (endPrecompIdx == ~0U)
    return IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  return IncrementalPDBTypeReplayBoundary::make<
      ReplayTypeRecordsSkippingEndPrecomp>(
      ReplayTypeRecordsSkippingEndPrecomp{endPrecompIdx});
}

static std::string
getTypeCompositeKey(const IncrementalPDBTypeReplaySnapshot &entry) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  entry.match(
      [&](const ReplayObjectTypes &object) {
        os << object.path << '\n' << object.parentPath << '\n'
           << object.archiveOffset << '\n'
           << unsigned(WireIncrementalPDBTypeReplayKind::Object);
      },
      [&](const ReplayPrecompiledHeaderTypes &pch) {
        os << pch.path << '\n' << pch.parentPath << '\n' << pch.archiveOffset
           << '\n'
           << unsigned(WireIncrementalPDBTypeReplayKind::PrecompiledHeader);
      },
      [&](const ReplayUsingPrecompiledHeaderTypes &usingPCH) {
        os << usingPCH.path << '\n' << usingPCH.parentPath << '\n'
           << usingPCH.archiveOffset << '\n'
           << unsigned(
                  WireIncrementalPDBTypeReplayKind::UsingPrecompiledHeader);
      },
      [&](const ReplayTypeServerTpiOnly &typeServer) {
        os << typeServer.path << '\n' << typeServer.parentPath << '\n'
           << typeServer.archiveOffset << '\n'
           << unsigned(WireIncrementalPDBTypeReplayKind::TypeServerTpiOnly);
      },
      [&](const ReplayTypeServerTpiAndIpi &typeServer) {
        os << typeServer.path << '\n' << typeServer.parentPath << '\n'
           << typeServer.archiveOffset << '\n'
           << unsigned(WireIncrementalPDBTypeReplayKind::TypeServerTpiAndIpi);
      });
  return std::string(buffer);
}

static std::string
getModuleCompositeKey(const CachedModuleReplay &entry) {
  return getCompositeKey(entry.path, entry.parentPath, entry.archiveOffset);
}

static SymbolReplayRouting cloneSymbolReplayRouting(
    const SymbolReplayRouting &routing) {
  return routing.match(
      [&](const EmitGlobalOnlySymbol &) {
        return SymbolReplayRouting::make<EmitGlobalOnlySymbol>();
      },
      [&](const EmitModuleOnlySymbol &) {
        return SymbolReplayRouting::make<EmitModuleOnlySymbol>();
      },
      [&](const EmitGlobalAndModuleSymbol &) {
        return SymbolReplayRouting::make<EmitGlobalAndModuleSymbol>();
      });
}

static GlobalSymbolReplay cloneGlobalSymbolReplay(
    const GlobalSymbolReplay &globalReplay) {
  return globalReplay.match(
      [&](const OmitGlobalReplay &) {
        return GlobalSymbolReplay::make<OmitGlobalReplay>();
      },
      [&](const ReplayGlobalSymbolBytes &) {
        return GlobalSymbolReplay::make<ReplayGlobalSymbolBytes>();
      },
      [&](const ReplayGlobalProcedureReference &) {
        return GlobalSymbolReplay::make<ReplayGlobalProcedureReference>();
      });
}

static SymbolRewritePlan cloneSymbolRewritePlan(
    const SymbolRewritePlan &rewrite) {
  return rewrite.match(
      [&](const ReplaySymbolWithoutTypeRewrite &) {
        return SymbolRewritePlan::make<ReplaySymbolWithoutTypeRewrite>();
      },
      [&](const ReplayProcIdEndSymbol &) {
        return SymbolRewritePlan::make<ReplayProcIdEndSymbol>();
      },
      [&](const ReplayProcIdWithFixedTypeIndex &) {
        return SymbolRewritePlan::make<ReplayProcIdWithFixedTypeIndex>();
      },
      [&](const ReplaySymbolWithDiscoveredTypeRefs &generic) {
        return SymbolRewritePlan::make<ReplaySymbolWithDiscoveredTypeRefs>(
            ReplaySymbolWithDiscoveredTypeRefs{generic.typeRefs});
      });
}

static SymbolScopeReplay cloneSymbolScopeReplay(
    const SymbolScopeReplay &scope) {
  return scope.match(
      [&](const ReplayStandaloneSymbol &) {
        return SymbolScopeReplay::make<ReplayStandaloneSymbol>();
      },
      [&](const ReplayScopeOpeningSymbol &) {
        return SymbolScopeReplay::make<ReplayScopeOpeningSymbol>();
      },
      [&](const ReplayScopeClosingSymbol &) {
        return SymbolScopeReplay::make<ReplayScopeClosingSymbol>();
      });
}

static CachedSymbolReplay cloneCachedSymbolReplay(const CachedSymbolReplay &symbol) {
  return CachedSymbolReplay{symbol.location, symbol.alignedLength,
                            cloneSymbolReplayRouting(symbol.routing),
                            cloneGlobalSymbolReplay(symbol.globalReplay),
                            cloneSymbolRewritePlan(symbol.rewrite),
                            cloneSymbolScopeReplay(symbol.scope)};
}

static IncrementalPDBSubsectionReplay
cloneSubsectionReplay(const IncrementalPDBSubsectionReplay &subsection) {
  return subsection.match(
      [&](const ReplayOpaqueSubsection &opaque) {
        return IncrementalPDBSubsectionReplay::make<ReplayOpaqueSubsection>(
            opaque);
      },
      [&](const ReplaySymbolSubsection &symbols) {
        ReplaySymbolSubsection cloned;
        cloned.location = symbols.location;
        cloned.symbols.reserve(symbols.symbols.size());
        for (const CachedSymbolReplay &symbol : symbols.symbols)
          cloned.symbols.push_back(cloneCachedSymbolReplay(symbol));
        return IncrementalPDBSubsectionReplay::make<ReplaySymbolSubsection>(
            std::move(cloned));
      });
}

static IncrementalPDBChunkReplay
cloneChunkReplay(const IncrementalPDBChunkReplay &chunk) {
  return chunk.match(
      [&](const ReplayDebugSChunk &debugS) {
        ReplayDebugSChunk cloned;
        cloned.chunkOrdinal = debugS.chunkOrdinal;
        cloned.subsections.reserve(debugS.subsections.size());
        for (const IncrementalPDBSubsectionReplay &subsection :
             debugS.subsections)
          cloned.subsections.push_back(cloneSubsectionReplay(subsection));
        return IncrementalPDBChunkReplay::make<ReplayDebugSChunk>(
            std::move(cloned));
      },
      [&](const ReplayDebugFChunk &debugF) {
        return IncrementalPDBChunkReplay::make<ReplayDebugFChunk>(debugF);
      });
}

static CachedModuleReplay
cloneCachedModuleReplayImpl(const CachedModuleReplay &entry) {
  CachedModuleReplay cloned;
  cloned.path = entry.path;
  cloned.parentPath = entry.parentPath;
  cloned.archiveOffset = entry.archiveOffset;
  cloned.debugSHash = entry.debugSHash;
  cloned.debugFHash = entry.debugFHash;
  cloned.relocHash = entry.relocHash;
  cloned.moduleStreamSize = entry.moduleStreamSize;
  cloned.stringFixups = entry.stringFixups;
  cloned.chunks.reserve(entry.chunks.size());
  for (const IncrementalPDBChunkReplay &chunk : entry.chunks)
    cloned.chunks.push_back(cloneChunkReplay(chunk));
  return cloned;
}

static IncrementalPDBTypeReplaySnapshot
cloneTypeReplay(const IncrementalPDBTypeReplaySnapshot &entry) {
  return entry.match(
      [&](const ReplayObjectTypes &object) {
        ReplayObjectTypes cloned{object.path,
                                 object.parentPath,
                                 object.archiveOffset,
                                 object.contentHash,
                                 cloneTypeReplayBoundary(object.boundary),
                                 object.ghashes,
                                 object.isItemIndexBits};
        return IncrementalPDBTypeReplaySnapshot::make<ReplayObjectTypes>(
            std::move(cloned));
      },
      [&](const ReplayPrecompiledHeaderTypes &pch) {
        ReplayPrecompiledHeaderTypes cloned{
            pch.path,      pch.parentPath,       pch.archiveOffset,
            pch.contentHash, pch.pchSignature, cloneTypeReplayBoundary(pch.boundary),
            pch.ghashes,   pch.isItemIndexBits};
        return IncrementalPDBTypeReplaySnapshot::make<
            ReplayPrecompiledHeaderTypes>(std::move(cloned));
      },
      [&](const ReplayUsingPrecompiledHeaderTypes &usingPCH) {
        ReplayUsingPrecompiledHeaderTypes cloned{
            usingPCH.path,
            usingPCH.parentPath,
            usingPCH.archiveOffset,
            usingPCH.contentHash,
            usingPCH.dependencyHash,
            cloneTypeReplayBoundary(usingPCH.boundary),
            usingPCH.ghashes,
            usingPCH.isItemIndexBits};
        return IncrementalPDBTypeReplaySnapshot::make<
            ReplayUsingPrecompiledHeaderTypes>(std::move(cloned));
      },
      [&](const ReplayTypeServerTpiOnly &typeServer) {
        ReplayTypeServerTpiOnly cloned{
            typeServer.path,
            typeServer.parentPath,
            typeServer.archiveOffset,
            typeServer.contentHash,
            cloneTypeReplayBoundary(typeServer.boundary),
            typeServer.ghashes,
            typeServer.isItemIndexBits};
        return IncrementalPDBTypeReplaySnapshot::make<
            ReplayTypeServerTpiOnly>(std::move(cloned));
      },
      [&](const ReplayTypeServerTpiAndIpi &typeServer) {
        ReplayTypeServerTpiAndIpi cloned{
            typeServer.path,
            typeServer.parentPath,
            typeServer.archiveOffset,
            typeServer.contentHash,
            cloneTypeReplayBoundary(typeServer.boundary),
            typeServer.ghashes,
            typeServer.isItemIndexBits,
            typeServer.auxGHashes,
            typeServer.auxIsItemIndexBits};
        return IncrementalPDBTypeReplaySnapshot::make<
            ReplayTypeServerTpiAndIpi>(std::move(cloned));
      });
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

CachedModuleReplay cloneCachedModuleReplay(const CachedModuleReplay &entry) {
  return cloneCachedModuleReplayImpl(entry);
}

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

  return ctx.incremental->match(
      [&](const IncrementalDisabled &) {
        return IncrementalPDBCacheRuntimeMode::BypassCache;
      },
      [&](const PendingFullImageBuild &) {
        return IncrementalPDBCacheRuntimeMode::BypassCache;
      },
      [&](const FullImageBuild &full) {
        return full.baselineEmission.match(
            [&](const EmitNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
            },
            [&](const SkipNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::BypassCache;
            });
      },
      [&](const StateBackedLink &loaded) {
        return loaded.baselineEmission.match(
            [&](const EmitNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
            },
            [&](const SkipNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::BypassCache;
            });
      },
      [&](const LayoutStableLink &validated) {
        return validated.baselineEmission.match(
            [&](const EmitNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::RecordOnlyCache;
            },
            [&](const SkipNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::BypassCache;
            });
      },
      [&](const ByteReuseLink &reuse) {
        return reuse.baselineEmission.match(
            [&](const EmitNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache;
            },
            [&](const SkipNextBaseline &) {
              return IncrementalPDBCacheRuntimeMode::ReplayOnlyCache;
            });
      });
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

Expected<IncrementalPDBCacheSnapshot> loadIncrementalPDBCache(StringRef path) {
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

  IncrementalPDBCacheSnapshot cache;
  cache.linkerBuildId = header.linkerBuildId;
  cache.hardConfigHash = header.hardConfigHash;

  cache.typeReplays.reserve(typeRecordsOrErr->size());
  for (const IncrementalPDBTypeEntryRecord &record : *typeRecordsOrErr) {
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

    std::vector<GloballyHashedType> ghashes(
        ghashBlobOrErr->size() / sizeof(GloballyHashedType));
    std::vector<uint8_t> isItemIndexBits(isItemBlobOrErr->begin(),
                                         isItemBlobOrErr->end());
    std::vector<GloballyHashedType> auxGHashes(
        auxGHashBlobOrErr->size() / sizeof(GloballyHashedType));
    std::vector<uint8_t> auxIsItemIndexBits(auxIsItemBlobOrErr->begin(),
                                            auxIsItemBlobOrErr->end());
    memcpy(ghashes.data(), ghashBlobOrErr->data(), ghashBlobOrErr->size());
    memcpy(auxGHashes.data(), auxGHashBlobOrErr->data(),
           auxGHashBlobOrErr->size());

    IncrementalPDBTypeReplayBoundary boundary =
        decodeWireEndPrecompIdx(record.endPrecompIdx);
    auto kindOrErr = decodeWireTypeReplayKind(uint8_t(record.kind));
    if (!kindOrErr)
      return kindOrErr.takeError();
    switch (*kindOrErr) {
    case WireIncrementalPDBTypeReplayKind::Object:
      cache.typeReplays.push_back(
          IncrementalPDBTypeReplaySnapshot::make<ReplayObjectTypes>(
              ReplayObjectTypes{pathOrErr->str(), parentOrErr->str(),
                                record.archiveOffset, record.contentHash,
                                std::move(boundary), std::move(ghashes),
                                std::move(isItemIndexBits)}));
      break;
    case WireIncrementalPDBTypeReplayKind::PrecompiledHeader:
      if (record.dependencyHash > UINT32_MAX)
        return createStringError(inconvertibleErrorCode(),
                                 "incremental PDB cache PCH signature is invalid");
      cache.typeReplays.push_back(
          IncrementalPDBTypeReplaySnapshot::make<ReplayPrecompiledHeaderTypes>(
              ReplayPrecompiledHeaderTypes{
                  pathOrErr->str(), parentOrErr->str(), record.archiveOffset,
                  record.contentHash, static_cast<uint32_t>(record.dependencyHash),
                  std::move(boundary), std::move(ghashes),
                  std::move(isItemIndexBits)}));
      break;
    case WireIncrementalPDBTypeReplayKind::UsingPrecompiledHeader:
      cache.typeReplays.push_back(
          IncrementalPDBTypeReplaySnapshot::make<
              ReplayUsingPrecompiledHeaderTypes>(
              ReplayUsingPrecompiledHeaderTypes{
                  pathOrErr->str(), parentOrErr->str(), record.archiveOffset,
                  record.contentHash, record.dependencyHash, std::move(boundary),
                  std::move(ghashes), std::move(isItemIndexBits)}));
      break;
    case WireIncrementalPDBTypeReplayKind::TypeServerTpiOnly:
      if (!auxGHashes.empty() || !auxIsItemIndexBits.empty())
        return createStringError(inconvertibleErrorCode(),
                                 "incremental PDB cache type replay has unexpected IPI data");
      cache.typeReplays.push_back(
          IncrementalPDBTypeReplaySnapshot::make<ReplayTypeServerTpiOnly>(
              ReplayTypeServerTpiOnly{
                  pathOrErr->str(), parentOrErr->str(), record.archiveOffset,
                  record.contentHash, std::move(boundary), std::move(ghashes),
                  std::move(isItemIndexBits)}));
      break;
    case WireIncrementalPDBTypeReplayKind::TypeServerTpiAndIpi:
      cache.typeReplays.push_back(
          IncrementalPDBTypeReplaySnapshot::make<ReplayTypeServerTpiAndIpi>(
              ReplayTypeServerTpiAndIpi{
                  pathOrErr->str(), parentOrErr->str(), record.archiveOffset,
                  record.contentHash, std::move(boundary), std::move(ghashes),
                  std::move(isItemIndexBits), std::move(auxGHashes),
                  std::move(auxIsItemIndexBits)}));
      break;
    }
  }

  cache.moduleReplays.reserve(moduleRecordsOrErr->size());
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

    CachedModuleReplay entry;
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
      auto chunkKindOrErr = decodeWireChunkReplayKind(uint8_t(chunkRecord.kind));
      if (!chunkKindOrErr)
        return chunkKindOrErr.takeError();
      switch (*chunkKindOrErr) {
      case WireIncrementalPDBChunkReplayKind::DebugS: {
        if (chunkRecord.subsectionStart > subsectionRecordsOrErr->size() ||
            subsectionRecordsOrErr->size() - chunkRecord.subsectionStart <
                chunkRecord.subsectionCount)
          return createStringError(
              inconvertibleErrorCode(),
              "incremental PDB cache debug chunk subsection range is invalid");

        ReplayDebugSChunk chunk;
        chunk.chunkOrdinal = chunkRecord.chunkOrdinal;
        for (const IncrementalPDBSubsectionPlanRecord &subsectionRecord :
             subsectionRecordsOrErr->slice(chunkRecord.subsectionStart,
                                           chunkRecord.subsectionCount)) {
          IncrementalPDBRecordLocation location{subsectionRecord.recordOffset,
                                                subsectionRecord.recordLength,
                                                subsectionRecord.relocIndex};
          if (subsectionRecord.kind != uint16_t(DebugSubsectionKind::Symbols)) {
            if (subsectionRecord.symbolPlanCount != 0)
              return createStringError(
                  inconvertibleErrorCode(),
                  "incremental PDB cache opaque subsection unexpectedly carries symbol plans");
            chunk.subsections.push_back(
                IncrementalPDBSubsectionReplay::make<ReplayOpaqueSubsection>(
                    ReplayOpaqueSubsection{
                        static_cast<DebugSubsectionKind>(
                            uint16_t(subsectionRecord.kind)),
                        location}));
            continue;
          }

          if (subsectionRecord.symbolPlanStart > symbolRecordsOrErr->size() ||
              symbolRecordsOrErr->size() - subsectionRecord.symbolPlanStart <
                  subsectionRecord.symbolPlanCount)
            return createStringError(
                inconvertibleErrorCode(),
                "incremental PDB cache symbol replay range is invalid");

          ReplaySymbolSubsection subsection;
          subsection.location = location;
          subsection.symbols.reserve(subsectionRecord.symbolPlanCount);
          for (const IncrementalPDBSymbolPlanRecord &symbolRecord :
               symbolRecordsOrErr->slice(subsectionRecord.symbolPlanStart,
                                         subsectionRecord.symbolPlanCount)) {
            if (symbolRecord.typeRefStart > typeRefRecordsOrErr->size() ||
                typeRefRecordsOrErr->size() - symbolRecord.typeRefStart <
                    symbolRecord.typeRefCount)
              return createStringError(
                  inconvertibleErrorCode(),
                  "incremental PDB cache symbol type-ref range is invalid");

            auto routingOrErr = [&]() -> Expected<SymbolReplayRouting> {
              switch (symbolRecord.destMask) {
              case 1:
                return SymbolReplayRouting::make<EmitGlobalOnlySymbol>();
              case 2:
                return SymbolReplayRouting::make<EmitModuleOnlySymbol>();
              case 3:
                return SymbolReplayRouting::make<EmitGlobalAndModuleSymbol>();
              default:
                return createStringError(
                    inconvertibleErrorCode(),
                    "incremental PDB cache symbol routing is invalid");
              }
            }();
            if (!routingOrErr)
              return routingOrErr.takeError();

            auto globalReplayOrErr = [&]() -> Expected<GlobalSymbolReplay> {
              if (symbolRecord.flags & ~uint8_t(1))
                return createStringError(
                    inconvertibleErrorCode(),
                    "incremental PDB cache symbol flags are invalid");
              if (!(symbolRecord.destMask & 1)) {
                if (symbolRecord.flags != 0)
                  return createStringError(
                      inconvertibleErrorCode(),
                      "incremental PDB cache symbol proc-ref flag without global routing");
                return GlobalSymbolReplay::make<OmitGlobalReplay>();
              }
              if (symbolRecord.flags == 0)
                return GlobalSymbolReplay::make<ReplayGlobalSymbolBytes>();
              return GlobalSymbolReplay::make<ReplayGlobalProcedureReference>();
            }();
            if (!globalReplayOrErr)
              return globalReplayOrErr.takeError();

            auto rewriteOrErr = [&]() -> Expected<SymbolRewritePlan> {
              switch (symbolRecord.rewriteKind) {
              case 0:
                if (symbolRecord.typeRefCount != 0)
                  return createStringError(
                      inconvertibleErrorCode(),
                      "incremental PDB cache type refs without rewrite plan");
                return SymbolRewritePlan::make<
                    ReplaySymbolWithoutTypeRewrite>();
              case 1:
                if (symbolRecord.typeRefCount != 0)
                  return createStringError(
                      inconvertibleErrorCode(),
                      "incremental PDB cache proc-id-end replay carries type refs");
                return SymbolRewritePlan::make<ReplayProcIdEndSymbol>();
              case 2:
                if (symbolRecord.typeRefCount != 0)
                  return createStringError(
                      inconvertibleErrorCode(),
                      "incremental PDB cache fixed-index replay carries type refs");
                return SymbolRewritePlan::make<
                    ReplayProcIdWithFixedTypeIndex>();
              case 3: {
                std::vector<IncrementalPDBTypeRef> refs;
                refs.reserve(symbolRecord.typeRefCount);
                for (const IncrementalPDBTypeRefRecord &typeRefRecord :
                     typeRefRecordsOrErr->slice(symbolRecord.typeRefStart,
                                               symbolRecord.typeRefCount))
                  refs.push_back({static_cast<TiRefKind>(uint8_t(typeRefRecord.kind)),
                                  typeRefRecord.offset, typeRefRecord.count});
                return SymbolRewritePlan::make<
                    ReplaySymbolWithDiscoveredTypeRefs>(
                    ReplaySymbolWithDiscoveredTypeRefs{std::move(refs)});
              }
              default:
                return createStringError(
                    inconvertibleErrorCode(),
                    "incremental PDB cache symbol rewrite kind is invalid");
              }
            }();
            if (!rewriteOrErr)
              return rewriteOrErr.takeError();

            auto scopeOrErr = [&]() -> Expected<SymbolScopeReplay> {
              switch (symbolRecord.scopeAction) {
              case 0:
                return SymbolScopeReplay::make<ReplayStandaloneSymbol>();
              case 1:
                return SymbolScopeReplay::make<ReplayScopeOpeningSymbol>();
              case 2:
                return SymbolScopeReplay::make<ReplayScopeClosingSymbol>();
              default:
                return createStringError(
                    inconvertibleErrorCode(),
                    "incremental PDB cache symbol scope action is invalid");
              }
            }();
            if (!scopeOrErr)
              return scopeOrErr.takeError();

            subsection.symbols.push_back(CachedSymbolReplay{
                {symbolRecord.recordOffset, symbolRecord.recordLength,
                 symbolRecord.relocIndex},
                symbolRecord.alignedLength, std::move(*routingOrErr),
                std::move(*globalReplayOrErr), std::move(*rewriteOrErr),
                std::move(*scopeOrErr)});
          }

          chunk.subsections.push_back(
              IncrementalPDBSubsectionReplay::make<ReplaySymbolSubsection>(
                  std::move(subsection)));
        }

        entry.chunks.push_back(
            IncrementalPDBChunkReplay::make<ReplayDebugSChunk>(
                std::move(chunk)));
        break;
      }
      case WireIncrementalPDBChunkReplayKind::DebugF:
        if (chunkRecord.subsectionCount != 0)
          return createStringError(
              inconvertibleErrorCode(),
              "incremental PDB cache debug$F replay unexpectedly carries subsections");
        entry.chunks.push_back(
            IncrementalPDBChunkReplay::make<ReplayDebugFChunk>(
                ReplayDebugFChunk{chunkRecord.chunkOrdinal}));
        break;
      }
    }

    for (const IncrementalPDBStringFixupRecord &fixupRecord :
         stringFixupRecordsOrErr->slice(record.stringFixupStart,
                                        record.stringFixupCount)) {
      IncrementalPDBStringFixup fixup;
      fixup.strTabOffset = fixupRecord.strTabOffset;
      fixup.symOffsetOfReference = fixupRecord.symOffsetOfReference;
      entry.stringFixups.push_back(fixup);
    }

    cache.moduleReplays.push_back(std::move(entry));
  }

  return cache;
}

Error writeIncrementalPDBCache(StringRef path,
                               const IncrementalPDBCacheSnapshot &cache) {
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

  typeRecords.reserve(cache.typeReplays.size());
  for (const IncrementalPDBTypeReplaySnapshot &entry : cache.typeReplays) {
    IncrementalPDBTypeEntryRecord record = {};
    entry.match(
        [&](const ReplayObjectTypes &object) {
          record.pathOffset = strings.add(object.path);
          record.parentPathOffset = strings.add(object.parentPath);
          record.archiveOffset = object.archiveOffset;
          record.kind = uint8_t(WireIncrementalPDBTypeReplayKind::Object);
          record.contentHash = object.contentHash;
          record.endPrecompIdx = encodeWireEndPrecompIdx(object.boundary);
          record.ghashes = appendBlob(blobArena, ArrayRef(object.ghashes));
          record.isItemIndexBits =
              appendBlob(blobArena, ArrayRef(object.isItemIndexBits));
        },
        [&](const ReplayPrecompiledHeaderTypes &pch) {
          record.pathOffset = strings.add(pch.path);
          record.parentPathOffset = strings.add(pch.parentPath);
          record.archiveOffset = pch.archiveOffset;
          record.kind =
              uint8_t(WireIncrementalPDBTypeReplayKind::PrecompiledHeader);
          record.contentHash = pch.contentHash;
          record.dependencyHash = pch.pchSignature;
          record.endPrecompIdx = encodeWireEndPrecompIdx(pch.boundary);
          record.ghashes = appendBlob(blobArena, ArrayRef(pch.ghashes));
          record.isItemIndexBits =
              appendBlob(blobArena, ArrayRef(pch.isItemIndexBits));
        },
        [&](const ReplayUsingPrecompiledHeaderTypes &usingPCH) {
          record.pathOffset = strings.add(usingPCH.path);
          record.parentPathOffset = strings.add(usingPCH.parentPath);
          record.archiveOffset = usingPCH.archiveOffset;
          record.kind = uint8_t(
              WireIncrementalPDBTypeReplayKind::UsingPrecompiledHeader);
          record.contentHash = usingPCH.contentHash;
          record.dependencyHash = usingPCH.dependencyHash;
          record.endPrecompIdx = encodeWireEndPrecompIdx(usingPCH.boundary);
          record.ghashes = appendBlob(blobArena, ArrayRef(usingPCH.ghashes));
          record.isItemIndexBits =
              appendBlob(blobArena, ArrayRef(usingPCH.isItemIndexBits));
        },
        [&](const ReplayTypeServerTpiOnly &typeServer) {
          record.pathOffset = strings.add(typeServer.path);
          record.parentPathOffset = strings.add(typeServer.parentPath);
          record.archiveOffset = typeServer.archiveOffset;
          record.kind =
              uint8_t(WireIncrementalPDBTypeReplayKind::TypeServerTpiOnly);
          record.contentHash = typeServer.contentHash;
          record.endPrecompIdx = encodeWireEndPrecompIdx(typeServer.boundary);
          record.ghashes = appendBlob(blobArena, ArrayRef(typeServer.ghashes));
          record.isItemIndexBits =
              appendBlob(blobArena, ArrayRef(typeServer.isItemIndexBits));
        },
        [&](const ReplayTypeServerTpiAndIpi &typeServer) {
          record.pathOffset = strings.add(typeServer.path);
          record.parentPathOffset = strings.add(typeServer.parentPath);
          record.archiveOffset = typeServer.archiveOffset;
          record.kind =
              uint8_t(WireIncrementalPDBTypeReplayKind::TypeServerTpiAndIpi);
          record.contentHash = typeServer.contentHash;
          record.endPrecompIdx = encodeWireEndPrecompIdx(typeServer.boundary);
          record.ghashes = appendBlob(blobArena, ArrayRef(typeServer.ghashes));
          record.isItemIndexBits =
              appendBlob(blobArena, ArrayRef(typeServer.isItemIndexBits));
          record.auxGHashes =
              appendBlob(blobArena, ArrayRef(typeServer.auxGHashes));
          record.auxIsItemIndexBits =
              appendBlob(blobArena, ArrayRef(typeServer.auxIsItemIndexBits));
        });
    typeRecords.push_back(record);
  }

  moduleRecords.reserve(cache.moduleReplays.size());
  for (const CachedModuleReplay &entry : cache.moduleReplays) {
    IncrementalPDBModuleEntryRecord record = {};
    record.pathOffset = strings.add(entry.path);
    record.parentPathOffset = strings.add(entry.parentPath);
    record.archiveOffset = entry.archiveOffset;
    record.debugSHash = entry.debugSHash;
    record.debugFHash = entry.debugFHash;
    record.relocHash = entry.relocHash;
    record.moduleStreamSize = entry.moduleStreamSize;

    record.chunkPlanStart = chunkRecords.size();
    record.subsectionPlanStart = subsectionRecords.size();
    record.symbolPlanStart = symbolRecords.size();
    record.typeRefStart = typeRefRecords.size();
    for (const IncrementalPDBChunkReplay &chunk : entry.chunks) {
      chunk.match(
          [&](const ReplayDebugSChunk &debugS) {
            IncrementalPDBChunkPlanRecord chunkRecord = {};
            chunkRecord.chunkOrdinal = debugS.chunkOrdinal;
            chunkRecord.kind =
                uint8_t(WireIncrementalPDBChunkReplayKind::DebugS);
            chunkRecord.subsectionStart = subsectionRecords.size();
            for (const IncrementalPDBSubsectionReplay &subsection :
                 debugS.subsections) {
              subsection.match(
                  [&](const ReplayOpaqueSubsection &opaque) {
                    IncrementalPDBSubsectionPlanRecord subsectionRecord = {};
                    subsectionRecord.kind = uint16_t(opaque.kind);
                    subsectionRecord.recordOffset =
                        opaque.location.recordOffset;
                    subsectionRecord.recordLength =
                        opaque.location.recordLength;
                    subsectionRecord.relocIndex = opaque.location.relocIndex;
                    subsectionRecord.symbolPlanStart = symbolRecords.size();
                    subsectionRecord.symbolPlanCount = 0;
                    subsectionRecords.push_back(subsectionRecord);
                  },
                  [&](const ReplaySymbolSubsection &symbols) {
                    IncrementalPDBSubsectionPlanRecord subsectionRecord = {};
                    subsectionRecord.kind =
                        uint16_t(DebugSubsectionKind::Symbols);
                    subsectionRecord.recordOffset =
                        symbols.location.recordOffset;
                    subsectionRecord.recordLength =
                        symbols.location.recordLength;
                    subsectionRecord.relocIndex = symbols.location.relocIndex;
                    subsectionRecord.symbolPlanStart = symbolRecords.size();
                    for (const CachedSymbolReplay &symbol : symbols.symbols) {
                      IncrementalPDBSymbolPlanRecord symbolRecord = {};
                      symbolRecord.recordOffset = symbol.location.recordOffset;
                      symbolRecord.recordLength = symbol.location.recordLength;
                      symbolRecord.alignedLength = symbol.alignedLength;
                      symbolRecord.relocIndex = symbol.location.relocIndex;
                      symbolRecord.typeRefStart = typeRefRecords.size();
                      symbol.routing.match(
                          [&](const EmitGlobalOnlySymbol &) {
                            symbolRecord.destMask = 1;
                          },
                          [&](const EmitModuleOnlySymbol &) {
                            symbolRecord.destMask = 2;
                          },
                          [&](const EmitGlobalAndModuleSymbol &) {
                            symbolRecord.destMask = 3;
                          });
                      symbol.globalReplay.match(
                          [&](const OmitGlobalReplay &) {
                            symbolRecord.flags = 0;
                          },
                          [&](const ReplayGlobalSymbolBytes &) {
                            symbolRecord.flags = 0;
                          },
                          [&](const ReplayGlobalProcedureReference &) {
                            symbolRecord.flags = 1;
                          });
                      symbol.scope.match(
                          [&](const ReplayStandaloneSymbol &) {
                            symbolRecord.scopeAction = 0;
                          },
                          [&](const ReplayScopeOpeningSymbol &) {
                            symbolRecord.scopeAction = 1;
                          },
                          [&](const ReplayScopeClosingSymbol &) {
                            symbolRecord.scopeAction = 2;
                          });
                      symbol.rewrite.match(
                          [&](const ReplaySymbolWithoutTypeRewrite &) {
                            symbolRecord.rewriteKind = 0;
                          },
                          [&](const ReplayProcIdEndSymbol &) {
                            symbolRecord.rewriteKind = 1;
                          },
                          [&](const ReplayProcIdWithFixedTypeIndex &) {
                            symbolRecord.rewriteKind = 2;
                          },
                          [&](const ReplaySymbolWithDiscoveredTypeRefs &generic) {
                            symbolRecord.rewriteKind = 3;
                            for (const IncrementalPDBTypeRef &ref :
                                 generic.typeRefs) {
                              IncrementalPDBTypeRefRecord typeRefRecord = {};
                              typeRefRecord.kind = uint8_t(ref.kind);
                              typeRefRecord.offset = ref.offset;
                              typeRefRecord.count = ref.count;
                              typeRefRecords.push_back(typeRefRecord);
                            }
                          });
                      symbolRecord.typeRefCount =
                          typeRefRecords.size() - symbolRecord.typeRefStart;
                      symbolRecords.push_back(symbolRecord);
                    }
                    subsectionRecord.symbolPlanCount =
                        symbolRecords.size() - subsectionRecord.symbolPlanStart;
                    subsectionRecords.push_back(subsectionRecord);
                  });
            }
            chunkRecord.subsectionCount =
                subsectionRecords.size() - chunkRecord.subsectionStart;
            chunkRecords.push_back(chunkRecord);
          },
          [&](const ReplayDebugFChunk &debugF) {
            IncrementalPDBChunkPlanRecord chunkRecord = {};
            chunkRecord.chunkOrdinal = debugF.chunkOrdinal;
            chunkRecord.kind =
                uint8_t(WireIncrementalPDBChunkReplayKind::DebugF);
            chunkRecord.subsectionStart = subsectionRecords.size();
            chunkRecord.subsectionCount = 0;
            chunkRecords.push_back(chunkRecord);
          });
    }
    record.chunkPlanCount = chunkRecords.size() - record.chunkPlanStart;
    record.subsectionPlanCount =
        subsectionRecords.size() - record.subsectionPlanStart;
    record.symbolPlanCount = symbolRecords.size() - record.symbolPlanStart;
    record.typeRefCount = typeRefRecords.size() - record.typeRefStart;

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

  Expected<IncrementalPDBCacheSnapshot> cacheOrErr =
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

  session->loadedCache = std::move(*cacheOrErr);
  for (const IncrementalPDBTypeReplaySnapshot &entry :
       session->loadedCache.typeReplays)
    session->loadedTypesByKey[getTypeCompositeKey(entry)] = &entry;
  for (const CachedModuleReplay &entry : session->loadedCache.moduleReplays)
    session->loadedModulesByKey[getModuleCompositeKey(entry)] = &entry;
  return session;
}

const IncrementalPDBTypeReplaySnapshot *
IncrementalPDBCacheSession::findLoadedTypeReplay(StringRef key,
                                                 const TpiSource &source) const {
  auto it = loadedTypesByKey.find(key);
  if (it == loadedTypesByKey.end())
    return nullptr;
  const IncrementalPDBTypeReplaySnapshot *entry = it->second;
  return matchesIncrementalPDBTypeReplay(source, *entry) ? entry : nullptr;
}

const CachedModuleReplay *
IncrementalPDBCacheSession::findLoadedModuleReplay(StringRef key,
                                                   const ObjFile &file) const {
  auto it = loadedModulesByKey.find(key);
  if (it == loadedModulesByKey.end())
    return nullptr;
  const CachedModuleReplay *entry = it->second;
  if (entry->debugSHash != computeIncrementalPDBModuleDebugSHash(file) ||
      entry->debugFHash != computeIncrementalPDBModuleDebugFHash(file) ||
      entry->relocHash != computeIncrementalPDBModuleRelocHash(file))
    return nullptr;
  return entry;
}

IncrementalPDBTypeReplayLookup
IncrementalPDBCacheSession::lookupTypeReplay(const TpiSource &source) {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
    return IncrementalPDBTypeReplayLookup::make<RebuildTypeFromCurrentInput>();
  }
  std::string key = getIncrementalPDBTypeCacheKey(source);
  if (key.empty())
    return IncrementalPDBTypeReplayLookup::make<RebuildTypeFromCurrentInput>();
  const IncrementalPDBTypeReplaySnapshot *entry =
      findLoadedTypeReplay(key, source);
  if (entry) {
    ++typeCacheHits;
    if (ctx.config.verbose)
      Log(ctx) << "pdbcache: type hit " << key;
    return IncrementalPDBTypeReplayLookup::make<ReplayTypeFromCache>(
        ReplayTypeFromCache{std::cref(*entry)});
  }
  ++typeCacheMisses;
  if (ctx.config.verbose)
    Log(ctx) << "pdbcache: type miss " << key;
  return IncrementalPDBTypeReplayLookup::make<RebuildTypeFromCurrentInput>();
}

IncrementalPDBModuleReplayLookup
IncrementalPDBCacheSession::lookupModuleReplay(const ObjFile &file) {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
    return IncrementalPDBModuleReplayLookup::make<
        RebuildModuleFromCurrentInput>();
  }
  const CachedModuleReplay *entry =
      findLoadedModuleReplay(getIncrementalPDBCacheObjectKey(file), file);
  if (entry) {
    ++moduleCacheHits;
    if (ctx.config.verbose)
      Log(ctx) << "pdbcache: module hit " << file.getName();
    return IncrementalPDBModuleReplayLookup::make<ReplayModuleFromCache>(
        ReplayModuleFromCache{std::cref(*entry)});
  }
  ++moduleCacheMisses;
  if (ctx.config.verbose)
    Log(ctx) << "pdbcache: module miss " << file.getName();
  return IncrementalPDBModuleReplayLookup::make<
      RebuildModuleFromCurrentInput>();
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
  recordedTypeReplays.clear();
  for (TpiSource *source : sources) {
    buildIncrementalPDBTypeReplay(*source).match(
        [&](const SkipRecordedTypeReplay &) {},
        [&](RecordTypeReplay replay) {
          recordedTypeReplays.push_back(std::move(replay.replay));
        });
  }
}

Error IncrementalPDBCacheSession::writeCache(
    const DenseMap<const ObjFile *, CachedModuleReplay> &modulePlans) const {
  switch (mode) {
  case IncrementalPDBCacheRuntimeMode::RecordOnlyCache:
  case IncrementalPDBCacheRuntimeMode::ReplayAndRecordCache:
    break;
  case IncrementalPDBCacheRuntimeMode::BypassCache:
  case IncrementalPDBCacheRuntimeMode::ReplayOnlyCache:
    return Error::success();
  }

  IncrementalPDBCacheSnapshot cache;
  cache.linkerBuildId = computeIncrementalPDBCacheBuildId();
  cache.hardConfigHash = computeIncrementalPDBCacheHardConfigHash(ctx.config);
  cache.typeReplays.reserve(recordedTypeReplays.size());
  for (const IncrementalPDBTypeReplaySnapshot &entry : recordedTypeReplays)
    cache.typeReplays.push_back(cloneTypeReplay(entry));
  cache.moduleReplays.reserve(modulePlans.size());
  for (const auto &it : modulePlans)
    cache.moduleReplays.push_back(cloneCachedModuleReplay(it.second));
  return writeIncrementalPDBCache(cachePath, cache);
}

} // namespace lld::coff
