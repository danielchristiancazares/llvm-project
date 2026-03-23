#include "Incremental.h"
#include "COFFLinkerContext.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::object;

namespace lld::coff {

namespace {

static bool hasBitcodeInputs(COFFLinkerContext &ctx) {
  bool hasBitcode = false;
  ctx.forEachSymtab([&](SymbolTable &symtab) {
    hasBitcode |= !symtab.bitcodeFileInstances.empty();
  });
  return hasBitcode;
}

static void appendSortedStrings(raw_ostream &os, const StringSet<> &set) {
  SmallVector<StringRef, 16> keys;
  keys.reserve(set.size());
  for (const auto &entry : set)
    keys.push_back(entry.getKey());
  llvm::sort(keys);
  for (StringRef key : keys)
    os << key << '\n';
}

static void appendSortedStringMap(raw_ostream &os,
                                  const StringMap<std::string> &map) {
  SmallVector<StringRef, 16> keys;
  keys.reserve(map.size());
  for (const auto &entry : map)
    keys.push_back(entry.getKey());
  llvm::sort(keys);
  for (StringRef key : keys)
    os << key << '=' << map.lookup(key) << '\n';
}

static void appendManifestInputs(raw_ostream &os,
                                 const std::vector<std::string> &inputs) {
  for (const std::string &input : inputs)
    os << input << '\n';
}

static void ensureIncrementalStatePath(Configuration &config) {
  if (!config.incrementalStatePath.empty() || config.outputFile.empty())
    return;
  config.incrementalStatePath = config.outputFile;
  sys::path::replace_extension(config.incrementalStatePath, ".llilk");
}

static IncrementalStateFile buildIncrementalState(COFFLinkerContext &ctx,
                                                  IncrementalLinkSession &session) {
  IncrementalStateFile state;
  state.machine = ctx.config.machine;
  state.outputPath = ctx.config.outputFile;
  state.hardConfigHash = computeIncrementalHardConfigHash(ctx.config);
  state.softConfigHash = computeIncrementalSoftConfigHash(ctx.config);

  ErrorOr<std::unique_ptr<MemoryBuffer>> output = MemoryBuffer::getFile(
      ctx.config.outputFile, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!output) {
    report_fatal_error(createFileError(ctx.config.outputFile,
                                       errorCodeToError(output.getError())));
  }
  StringRef outputData = (*output)->getBuffer();
  state.outputSize = outputData.size();
  state.outputHash = xxh3_64bits(outputData);

  if (outputData.size() >= sizeof(dos_header)) {
    const uint8_t *buf =
        reinterpret_cast<const uint8_t *>(outputData.data());
    const dos_header *dos = reinterpret_cast<const dos_header *>(buf);
    uint32_t peOff = dos->AddressOfNewExeHeader;
    uint32_t headerOff =
        peOff + sizeof(llvm::COFF::PEMagic) + sizeof(coff_file_header);
    if (ctx.config.is64() &&
        outputData.size() >= headerOff + sizeof(pe32plus_header)) {
      const pe32plus_header *pe =
          reinterpret_cast<const pe32plus_header *>(buf + headerOff);
      state.sizeOfHeaders = pe->SizeOfHeaders;
      state.sizeOfImage = pe->SizeOfImage;
    } else if (outputData.size() >= headerOff + sizeof(pe32_header)) {
      const pe32_header *pe =
          reinterpret_cast<const pe32_header *>(buf + headerOff);
      state.sizeOfHeaders = pe->SizeOfHeaders;
      state.sizeOfImage = pe->SizeOfImage;
    }
  }

  state.inputs.reserve(ctx.objFileInstances.size());
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    IncrementalInputState input;
    input.name = session.currentInputNames[i];
    input.parentName = session.currentParentNames[i];
    input.contentHash = session.currentInputHashes[i];
    input.size = ctx.objFileInstances[i]->mb.getBufferSize();
    state.inputs.push_back(std::move(input));
  }

  for (OutputSection *section : ctx.outputSections) {
    IncrementalSectionState sectionState;
    sectionState.name = section->name.str();
    sectionState.characteristics = section->header.Characteristics;
    sectionState.rva = section->getRVA();
    sectionState.fileOffset = section->getFileOff();
    sectionState.virtualSize = section->getVirtualSize();
    sectionState.rawSize = section->getRawSize();
    sectionState.firstChunk = state.chunks.size();
    sectionState.chunkCount = section->chunks.size();
    state.sections.push_back(sectionState);

    uint64_t sectionEnd = section->getRVA() + section->getVirtualSize();
    for (size_t i = 0; i < section->chunks.size(); ++i) {
      Chunk *chunk = section->chunks[i];
      IncrementalChunkState chunkState;
      chunkState.kind = classifyIncrementalChunk(*chunk);
      chunkState.key = getIncrementalChunkKey(session, *chunk);
      chunkState.sectionIndex = state.sections.size() - 1;
      chunkState.outputCharacteristics = chunk->getOutputCharacteristics();
      chunkState.alignment = chunk->getAlignment();
      chunkState.rva = chunk->getRVA();
      chunkState.size = chunk->getSize();
      if (i + 1 < section->chunks.size())
        chunkState.slotCapacity =
            section->chunks[i + 1]->getRVA() - chunk->getRVA();
      else
        chunkState.slotCapacity = sectionEnd - chunk->getRVA();

      if (auto *sec = dyn_cast<SectionChunk>(chunk)) {
        auto it = session.inputIndices.find(sec->file);
        if (it != session.inputIndices.end())
          chunkState.inputIndex = it->second;
        chunkState.sectionNumber = sec->getSectionNumber();
        chunkState.contentHash = xxh3_64bits(sec->getContents());
        chunkState.symbolHash = computeIncrementalSymbolHash(*sec);
      }
      state.chunks.push_back(std::move(chunkState));
    }
  }

  return state;
}

} // namespace

StringRef incrementalFallbackReasonToString(IncrementalFallbackReason reason) {
  switch (reason) {
  case IncrementalFallbackReason::None:
    return "None";
  case IncrementalFallbackReason::MissingState:
    return "MissingState";
  case IncrementalFallbackReason::InvalidState:
    return "InvalidState";
  case IncrementalFallbackReason::UnsupportedMachine:
    return "UnsupportedMachine";
  case IncrementalFallbackReason::LtoInput:
    return "LtoInput";
  case IncrementalFallbackReason::TailMergeEnabled:
    return "TailMergeEnabled";
  case IncrementalFallbackReason::ConfigChanged:
    return "ConfigChanged";
  case IncrementalFallbackReason::OutputMismatch:
    return "OutputMismatch";
  case IncrementalFallbackReason::LayoutChanged:
    return "LayoutChanged";
  case IncrementalFallbackReason::SlotOverflow:
    return "SlotOverflow";
  }
  llvm_unreachable("unknown incremental fallback reason");
}

void setIncrementalFallback(COFFLinkerContext &ctx,
                            IncrementalFallbackReason reason,
                            const Twine &detail) {
  ctx.config.incrementalLinkActive = false;
  ctx.config.incrementalFallbackReason = reason;
  ctx.config.incrementalFallbackDetail = detail.str();
  if (!ctx.config.verbose || reason == IncrementalFallbackReason::None)
    return;
  Log(ctx) << "incremental: fallback: "
           << incrementalFallbackReasonToString(reason);
  if (!ctx.config.incrementalFallbackDetail.empty())
    Log(ctx) << "incremental: detail: " << ctx.config.incrementalFallbackDetail;
}

uint64_t computeIncrementalHardConfigHash(const Configuration &config) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  os << uint32_t(config.machine) << '\n'
     << config.dll << '\n'
     << config.noEntry << '\n'
     << config.align << '\n'
     << config.fileAlign << '\n'
     << config.imageBase << '\n'
     << config.dynamicBase << '\n'
     << config.largeAddressAware << '\n'
     << config.highEntropyVA << '\n'
     << config.guardCF << '\n'
     << config.hotpatchCompat << '\n'
     << config.functionPadMin << '\n'
     << config.manifest << '\n'
     << config.manifestID << '\n'
     << config.manifestUAC << '\n'
     << config.manifestLevel << '\n'
     << config.manifestUIAccess << '\n'
     << config.manifestFile << '\n';
  for (const auto &entry : config.merge)
    os << "merge:" << entry.first << '=' << entry.second << '\n';
  for (const auto &entry : config.section)
    os << "section:" << entry.first << '=' << entry.second << '\n';
  for (const auto &entry : config.sectionOrder)
    os << "sectionorder:" << entry.first << '=' << entry.second << '\n';
  appendSortedStrings(os, config.delayLoads);
  appendManifestInputs(os, config.manifestInput);
  return xxh3_64bits(buffer);
}

uint64_t computeIncrementalSoftConfigHash(const Configuration &config) {
  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  os << config.pdbPath << '\n'
     << config.pdbAltPath << '\n'
     << config.pdbSourcePath << '\n'
     << config.pdbPageSize << '\n'
     << config.lldmapFile << '\n'
     << config.mapFile << '\n'
     << config.timestamp << '\n'
     << config.repro << '\n'
     << unsigned(config.buildIDHash) << '\n';
  for (const std::string &natvis : config.natvisFiles)
    os << natvis << '\n';
  appendSortedStringMap(os, config.namedStreams);
  return xxh3_64bits(buffer);
}

bool prepareCurrentIncrementalInputs(COFFLinkerContext &ctx,
                                     IncrementalLinkSession &session) {
  session.currentInputHashes.clear();
  session.currentInputNames.clear();
  session.currentParentNames.clear();
  session.inputIndices.clear();
  session.changedInputs.clear();
  session.reusedChunkData.clear();
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    ObjFile *file = ctx.objFileInstances[i];
    session.inputIndices[file] = i;
    session.currentInputHashes.push_back(xxh3_64bits(file->mb.getBuffer()));
    session.currentInputNames.push_back(file->getName().str());
    session.currentParentNames.push_back(file->parentName.str());
  }
  return true;
}

IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk) {
  if (auto *section = dyn_cast<SectionChunk>(&chunk))
    if (section->file)
      return IncrementalChunkKind::ObjSection;
  return IncrementalChunkKind::Synthetic;
}

std::string getIncrementalChunkKey(const IncrementalLinkSession &session,
                                   const Chunk &chunk) {
  std::string key;
  raw_string_ostream os(key);
  if (auto *section = dyn_cast<SectionChunk>(&chunk)) {
    auto it = session.inputIndices.find(section->file);
    if (it != session.inputIndices.end())
      os << "obj:" << it->second << ':';
    else
      os << "obj:?:";
    if (section->sym)
      os << "comdat:" << section->sym->getName();
    else
      os << "sec:" << section->getSectionNumber() << ':'
         << section->getSectionName();
    return os.str();
  }
  os << "syn:" << chunk.getDebugName() << ':' << chunk.getOutputCharacteristics()
     << ':' << chunk.getAlignment();
  return os.str();
}

uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk) {
  SmallVector<std::pair<std::string, uint32_t>, 8> symbols;
  uint32_t sectionNumber = chunk.getSectionNumber();
  for (const SymbolRef &ref : chunk.file->getCOFFObj()->symbols()) {
    COFFSymbolRef symbol = chunk.file->getCOFFObj()->getCOFFSymbol(ref);
    if (!symbol.isExternal())
      continue;
    Expected<StringRef> nameOrErr = chunk.file->getCOFFObj()->getSymbolName(symbol);
    if (!nameOrErr)
      continue;
    if (symbol.getSectionNumber() != static_cast<int32_t>(sectionNumber))
      continue;
    symbols.emplace_back(nameOrErr->str(), symbol.getValue());
  }
  llvm::sort(symbols, [](const auto &lhs, const auto &rhs) {
    return lhs < rhs;
  });

  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  for (const auto &entry : symbols)
    os << entry.first << '=' << entry.second << '\n';
  return xxh3_64bits(buffer);
}

void prepareIncrementalLink(COFFLinkerContext &ctx) {
  ctx.config.incrementalLinkActive = false;
  ctx.config.incrementalLinkEligible = false;
  ctx.config.incrementalFallbackReason = IncrementalFallbackReason::None;
  ctx.config.incrementalFallbackDetail.clear();
  ctx.incrementalSession.reset();

  if (!ctx.config.incrementalLinkRequested)
    return;

  ensureIncrementalStatePath(ctx.config);
  auto session = std::make_unique<IncrementalLinkSession>();
  session->canWriteState = true;
  prepareCurrentIncrementalInputs(ctx, *session);

  if (ctx.config.machine != AMD64) {
    session->canWriteState = false;
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::UnsupportedMachine);
    return;
  }

  if (hasBitcodeInputs(ctx)) {
    session->canWriteState = false;
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::LtoInput);
    return;
  }

  if (ctx.config.tailMerge) {
    session->canWriteState = false;
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::TailMergeEnabled);
    return;
  }

  ctx.config.incrementalLinkEligible = true;

  if (!sys::fs::exists(ctx.config.incrementalStatePath)) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::MissingState);
    return;
  }

  Expected<IncrementalStateFile> stateOrErr =
      loadIncrementalState(ctx.config.incrementalStatePath);
  if (!stateOrErr) {
    std::string message = toString(stateOrErr.takeError());
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState, message);
    return;
  }

  uint64_t hardHash = computeIncrementalHardConfigHash(ctx.config);
  uint64_t softHash = computeIncrementalSoftConfigHash(ctx.config);
  if (stateOrErr->machine != ctx.config.machine ||
      stateOrErr->hardConfigHash != hardHash) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::ConfigChanged);
    return;
  }

  if (stateOrErr->inputs.size() != session->currentInputHashes.size()) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged);
    return;
  }

  for (size_t i = 0; i < stateOrErr->inputs.size(); ++i) {
    const IncrementalInputState &input = stateOrErr->inputs[i];
    if (input.name != session->currentInputNames[i] ||
        input.parentName != session->currentParentNames[i]) {
      ctx.incrementalSession = std::move(session);
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged);
      return;
    }
    if (input.contentHash != session->currentInputHashes[i])
      session->changedInputs.insert(ctx.objFileInstances[i]);
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> oldImage = MemoryBuffer::getFile(
      ctx.config.outputFile, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!oldImage) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::OutputMismatch);
    return;
  }
  if ((*oldImage)->getBufferSize() != stateOrErr->outputSize ||
      xxh3_64bits((*oldImage)->getBuffer()) != stateOrErr->outputHash) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::OutputMismatch);
    return;
  }

  session->state = std::move(*stateOrErr);
  session->oldImage = std::move(*oldImage);
  session->stateLoaded = true;
  session->softConfigChanged =
      session->state.softConfigHash != softHash;
  ctx.incrementalSession = std::move(session);
  if (ctx.config.verbose)
    Log(ctx) << "incremental: using state " << ctx.config.incrementalStatePath;
}

void finalizeIncrementalLink(COFFLinkerContext &ctx) {
  if (errorCount() != 0 || !ctx.incrementalSession ||
      !ctx.incrementalSession->canWriteState)
    return;

  IncrementalStateFile state =
      buildIncrementalState(ctx, *ctx.incrementalSession);
  if (Error err = writeIncrementalState(ctx.config.incrementalStatePath, state))
    Warn(ctx) << "failed to write incremental state: " << toString(std::move(err));
}

} // namespace lld::coff
