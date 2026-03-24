#include "Incremental.h"
#include "COFFLinkerContext.h"
#include "IncrementalRedirects.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "Writer.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
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

static std::string getIncrementalArchiveMemberKey(StringRef archiveName,
                                                  uint64_t archiveOffset,
                                                  StringRef memberName) {
  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  os << archiveName << '\n' << archiveOffset << '\n' << memberName;
  return std::string(buffer);
}

static uint64_t computeIncrementalImportTopologyHash(COFFLinkerContext &ctx) {
  SmallVector<std::string, 16> records;
  records.reserve(ctx.importFileInstances.size());
  for (ImportFile *file : ctx.importFileInstances) {
    std::string record;
    raw_string_ostream os(record);
    os << file->dllName << '\n'
       << file->externalName << '\n'
       << file->hdr->OrdinalHint << '\n'
       << file->hdr->TypeInfo << '\n';
    records.push_back(std::move(record));
  }
  llvm::sort(records);

  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  for (const std::string &record : records)
    os << record;
  return xxh3_64bits(buffer);
}

static uint64_t computeIncrementalExportTopologyHash(COFFLinkerContext &ctx) {
  SmallVector<std::string, 16> records;
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    for (const Export &e : symtab.exports) {
      std::string record;
      raw_string_ostream os(record);
      os << e.name << '\n'
         << e.extName << '\n'
         << e.exportAs << '\n'
         << e.importName << '\n'
         << e.forwardTo << '\n'
         << e.exportName << '\n'
         << e.symbolName << '\n'
         << e.ordinal << '\n'
         << e.noname << '\n'
         << e.data << '\n'
         << e.isPrivate << '\n'
         << e.constant << '\n'
         << unsigned(e.source) << '\n';
      records.push_back(std::move(record));
    }
  });
  llvm::sort(records);

  SmallString<512> buffer;
  raw_svector_ostream os(buffer);
  for (const std::string &record : records)
    os << record;
  return xxh3_64bits(buffer);
}

static uint64_t computeIncrementalResourceInputHash(COFFLinkerContext &ctx) {
  SmallVector<std::string, 8> records;
  for (MemoryBufferRef mb : ctx.driver.getResources()) {
    std::string record;
    raw_string_ostream os(record);
    os << mb.getBufferIdentifier() << '\n'
       << xxh3_64bits(mb.getBuffer()) << '\n';
    records.push_back(std::move(record));
  }
  llvm::sort(records);

  SmallString<256> buffer;
  raw_svector_ostream os(buffer);
  for (const std::string &record : records)
    os << record;
  return xxh3_64bits(buffer);
}

static void buildIncrementalLayoutTables(COFFLinkerContext &ctx,
                                         IncrementalLinkSession &session,
                                         IncrementalStateFile &state) {
  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections)
    if (section->getVirtualSize() != 0)
      activeSections.push_back(section);

  DenseMap<const OutputSection *, uint32_t> envelopeIndices;
  for (size_t sectionIndex = 0; sectionIndex < activeSections.size();
       ++sectionIndex) {
    OutputSection *section = activeSections[sectionIndex];
    IncrementalSlotClass slotClass = classifyIncrementalSection(
        section->name, section->header.Characteristics);
    if (slotClass == IncrementalSlotClass::None)
      continue;

    IncrementalSectionEnvelopeState envelope;
    envelope.name = section->name.str();
    envelope.characteristics = section->header.Characteristics;
    envelope.sectionRVA = section->getRVA();
    envelope.maxSectionEndRVA =
        sectionIndex + 1 < activeSections.size()
            ? activeSections[sectionIndex + 1]->getRVA()
            : state.sizeOfImage;
    envelope.slotClass = slotClass;
    envelope.packedActivePrefix = isIncrementalPackedClass(slotClass);
    envelope.slotReuseEnabled = isIncrementalSlotReuseClass(slotClass);
    uint64_t activeEndRVA = section->getRVA() + section->getVirtualSize();
    if (slotClass == IncrementalSlotClass::Text) {
      activeEndRVA = section->getRVA();
      for (Chunk *chunk : section->chunks) {
        if (isa<IncrementalLongThunkChunkX64>(chunk))
          continue;
        activeEndRVA = std::max(activeEndRVA, chunk->getRVA() + chunk->getSize());
      }
    }
    envelope.activeEndRVA = activeEndRVA;
    envelopeIndices[section] = state.sectionEnvelopes.size();
    state.sectionEnvelopes.push_back(std::move(envelope));
  }

  for (OutputSection *section : activeSections) {
    auto envelopeIt = envelopeIndices.find(section);
    if (envelopeIt == envelopeIndices.end())
      continue;

    uint32_t envelopeIndex = envelopeIt->second;
    const IncrementalSectionEnvelopeState &envelope =
        state.sectionEnvelopes[envelopeIndex];

    if (envelope.slotReuseEnabled) {
      auto disableStateWrite = [&](const Twine &detail) {
        session.canWriteState = false;
        if (ctx.config.verbose)
          Log(ctx) << "incremental: not writing state: " << detail;
      };
      auto isPersistedSlotChunk = [&](Chunk *chunk) {
        return isIncrementalPersistedSlotChunk(envelope.slotClass, *chunk);
      };
      for (size_t chunkIndex = 0; chunkIndex < section->chunks.size();
           ++chunkIndex) {
        Chunk *chunk = section->chunks[chunkIndex];
        if (!isPersistedSlotChunk(chunk))
          continue;
        std::string key = getIncrementalChunkKey(session, *chunk);
        bool isPadding = isa<IncrementalPaddingChunk>(chunk);

        uint64_t slotEnd = envelope.activeEndRVA;
        for (size_t nextIndex = chunkIndex + 1; nextIndex < section->chunks.size();
             ++nextIndex) {
          Chunk *nextChunk = section->chunks[nextIndex];
          if (!isPersistedSlotChunk(nextChunk))
            continue;
          slotEnd = nextChunk->getRVA();
          break;
        }
        if (slotEnd < chunk->getRVA() ||
            slotEnd - chunk->getRVA() < chunk->getSize()) {
          disableStateWrite("slot table builder found an invalid persisted range");
          return;
        }

        IncrementalSlotRecordState slot;
        slot.envelopeIndex = envelopeIndex;
        slot.startRVA = chunk->getRVA();
        slot.capacity = slotEnd - chunk->getRVA();
        slot.committedSize = chunk->getSize();
        slot.minAlignment = chunk->getAlignment();
        slot.fillByte = isPadding ? cast<IncrementalPaddingChunk>(chunk)->getFillByte()
                                  : getIncrementalFillByte(envelope.slotClass);
        slot.state = isPadding ? IncrementalSlotState::Free
                               : IncrementalSlotState::Occupied;
        if (!isPadding)
          slot.occupantKey = key;
        state.slotRecords.push_back(std::move(slot));

        if (isPadding)
          continue;

        IncrementalPlacementState placement;
        placement.key = key;
        placement.envelopeIndex = envelopeIndex;
        placement.kind = IncrementalPlacementKind::ExistingSlot;
        placement.startRVA = chunk->getRVA();
        placement.size = chunk->getSize();
        placement.alignment = chunk->getAlignment();
        state.placements.push_back(std::move(placement));
      }
      continue;
    }

    if (!envelope.packedActivePrefix)
      continue;

    IncrementalPackedSectionState packedSection;
    packedSection.envelopeIndex = envelopeIndex;
    packedSection.activePrefixSize =
        envelope.activeEndRVA - envelope.sectionRVA;
    packedSection.reserveSize =
        envelope.maxSectionEndRVA - envelope.activeEndRVA;
    for (Chunk *chunk : section->chunks) {
      if (chunk->getSize() == 0)
        continue;
      std::string key = getIncrementalChunkKey(session, *chunk);
      packedSection.recordKeys.push_back(key);

      IncrementalPlacementState placement;
      placement.key = key;
      placement.envelopeIndex = envelopeIndex;
      placement.kind = IncrementalPlacementKind::PackedPrefix;
      placement.startRVA = chunk->getRVA();
      placement.size = chunk->getSize();
      placement.alignment = chunk->getAlignment();
      state.placements.push_back(std::move(placement));
    }
    state.packedSections.push_back(std::move(packedSection));
  }
}

static std::vector<IncrementalSymbolState>
buildIncrementalSymbolStates(COFFLinkerContext &ctx,
                             IncrementalLinkSession &session) {
  std::vector<IncrementalSymbolState> states;
  ctx.forEachActiveSymtab([&](SymbolTable &symtab) {
    symtab.forEachSymbol([&](Symbol *sym) {
      auto *def = dyn_cast<Defined>(sym);
      if (!def)
        return;
      if (auto *coff = dyn_cast<DefinedCOFF>(sym))
        if (!coff->getCOFFSymbol().isExternal())
          return;

      IncrementalSymbolState state;
      state.name = sym->getName().str();

      if (auto *reg = dyn_cast<DefinedRegular>(sym)) {
        state.kind = IncrementalSymbolKind::Regular;
        if (auto *file = dyn_cast<ObjFile>(reg->getFile()))
          if (auto it = session.inputIndices.find(file);
              it != session.inputIndices.end())
            state.inputIndex = it->second;
        state.value = reg->getValue();
        if (SectionChunk *chunk = reg->getChunk())
          state.auxiliaryKey = getIncrementalChunkKey(session, *chunk);
      } else if (auto *common = dyn_cast<DefinedCommon>(sym)) {
        state.kind = IncrementalSymbolKind::Common;
        if (auto *file = dyn_cast<ObjFile>(common->getFile()))
          if (auto it = session.inputIndices.find(file);
              it != session.inputIndices.end())
            state.inputIndex = it->second;
        state.value = common->getChunk()->getSize();
        std::string key;
        raw_string_ostream os(key);
        os << common->getChunk()->getAlignment();
        state.auxiliaryKey = os.str();
      } else if (auto *imp = dyn_cast<DefinedImportData>(sym)) {
        state.kind = IncrementalSymbolKind::ImportData;
        state.value = imp->getOrdinal();
        std::string key;
        raw_string_ostream os(key);
        os << imp->getDLLName() << '\n' << imp->getExternalName() << '\n'
           << imp->file->hdr->TypeInfo;
        state.auxiliaryKey = os.str();
      } else if (auto *thunk = dyn_cast<DefinedImportThunk>(sym)) {
        state.kind = IncrementalSymbolKind::ImportThunk;
        state.auxiliaryKey = thunk->wrappedSym->getName().str();
      } else if (auto *localImport = dyn_cast<DefinedLocalImport>(sym)) {
        state.kind = IncrementalSymbolKind::LocalImport;
        if (Chunk *chunk = localImport->getChunk())
          state.auxiliaryKey = chunk->getDebugName().str();
      } else if (auto *absolute = dyn_cast<DefinedAbsolute>(sym)) {
        state.kind = IncrementalSymbolKind::Absolute;
        state.value = absolute->getVA();
      } else if (auto *synthetic = dyn_cast<DefinedSynthetic>(sym)) {
        state.kind = IncrementalSymbolKind::Synthetic;
        if (Chunk *chunk = synthetic->getChunk())
          state.auxiliaryKey = chunk->getDebugName().str();
      } else {
        return;
      }

      states.push_back(std::move(state));
    });
  });

  llvm::sort(states, [](const IncrementalSymbolState &lhs,
                        const IncrementalSymbolState &rhs) {
    return std::tie(lhs.name, lhs.kind, lhs.inputIndex, lhs.value,
                    lhs.auxiliaryKey) <
           std::tie(rhs.name, rhs.kind, rhs.inputIndex, rhs.value,
                    rhs.auxiliaryKey);
  });
  return states;
}

static IncrementalStateFile buildIncrementalState(COFFLinkerContext &ctx,
                                                  IncrementalLinkSession &session) {
  IncrementalStateFile state;
  state.version = 3;
  state.layoutMode = IncrementalLayoutMode::Slotted;
  state.machine = ctx.config.machine;
  state.outputPath = ctx.config.outputFile;
  state.hardConfigHash = computeIncrementalHardConfigHash(ctx.config);
  state.softConfigHash = computeIncrementalSoftConfigHash(ctx.config);
  state.importTopologyHash = computeIncrementalImportTopologyHash(ctx);
  state.exportTopologyHash = computeIncrementalExportTopologyHash(ctx);
  state.resourceInputHash = computeIncrementalResourceInputHash(ctx);

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
    input.archiveOffset = session.currentArchiveOffsets[i];
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

  buildIncrementalLayoutTables(ctx, session, state);
  state.textRedirects = session.currentTextRedirects;
  state.textThunkPool = session.currentTextThunkPool;
  state.symbols = buildIncrementalSymbolStates(ctx, session);

  return state;
}

} // namespace

IncrementalSlotClass classifyIncrementalSection(StringRef name,
                                                uint32_t characteristics) {
  if (name == ".pdata")
    return IncrementalSlotClass::PDataPacked;
  if (name == ".xdata")
    return IncrementalSlotClass::XDataPacked;
  if (name == ".text")
    return IncrementalSlotClass::Text;
  if (name == ".rdata")
    return IncrementalSlotClass::RData;
  if (name == ".data")
    return IncrementalSlotClass::Data;

  if ((characteristics & llvm::COFF::IMAGE_SCN_CNT_CODE) &&
      (characteristics & llvm::COFF::IMAGE_SCN_MEM_EXECUTE))
    return IncrementalSlotClass::Text;
  return IncrementalSlotClass::None;
}

bool isIncrementalSlotReuseClass(IncrementalSlotClass slotClass) {
  switch (slotClass) {
  case IncrementalSlotClass::Text:
  case IncrementalSlotClass::RData:
  case IncrementalSlotClass::Data:
    return true;
  case IncrementalSlotClass::None:
  case IncrementalSlotClass::PDataPacked:
  case IncrementalSlotClass::XDataPacked:
    return false;
  }
  llvm_unreachable("unknown incremental slot class");
}

bool isIncrementalPackedClass(IncrementalSlotClass slotClass) {
  switch (slotClass) {
  case IncrementalSlotClass::PDataPacked:
  case IncrementalSlotClass::XDataPacked:
    return true;
  case IncrementalSlotClass::None:
  case IncrementalSlotClass::Text:
  case IncrementalSlotClass::RData:
  case IncrementalSlotClass::Data:
    return false;
  }
  llvm_unreachable("unknown incremental slot class");
}

uint8_t getIncrementalFillByte(IncrementalSlotClass slotClass) {
  switch (slotClass) {
  case IncrementalSlotClass::Text:
    return 0xCC;
  case IncrementalSlotClass::RData:
  case IncrementalSlotClass::Data:
  case IncrementalSlotClass::PDataPacked:
  case IncrementalSlotClass::XDataPacked:
  case IncrementalSlotClass::None:
    return 0x00;
  }
  llvm_unreachable("unknown incremental slot class");
}

bool isIncrementalPersistedSlotChunk(IncrementalSlotClass slotClass,
                                     const Chunk &chunk) {
  if (chunk.getSize() == 0)
    return false;
  return slotClass != IncrementalSlotClass::Text ||
         !isa<IncrementalLongThunkChunkX64>(&chunk);
}

std::optional<size_t> findBestFitIncrementalFreeSlot(
    ArrayRef<IncrementalSlotRecordState> slots, uint64_t size,
    uint32_t alignment) {
  std::optional<size_t> bestIndex;
  for (size_t i = 0; i < slots.size(); ++i) {
    const IncrementalSlotRecordState &slot = slots[i];
    if (slot.capacity < size || slot.startRVA % alignment != 0)
      continue;
    if (!bestIndex || slot.capacity < slots[*bestIndex].capacity ||
        (slot.capacity == slots[*bestIndex].capacity &&
         slot.startRVA < slots[*bestIndex].startRVA))
      bestIndex = i;
  }
  return bestIndex;
}

std::optional<uint64_t> allocateIncrementalTailReserve(uint64_t tailCursor,
                                                       uint64_t maxSectionEndRVA,
                                                       uint64_t size,
                                                       uint32_t alignment) {
  uint64_t startRVA = alignTo(tailCursor, uint64_t(alignment));
  if (startRVA > maxSectionEndRVA || maxSectionEndRVA - startRVA < size)
    return std::nullopt;
  return startRVA;
}

std::optional<uint64_t>
chooseIncrementalTextThunkRVA(uint64_t oldPoolThunkRVA, uint64_t tailCursor,
                              uint64_t poolCursor, uint64_t poolEndRVA,
                              ArrayRef<uint64_t> claimedThunkRVAs,
                              ArrayRef<uint64_t> freedThunkRVAs) {
  constexpr uint64_t thunkSize = 16;
  auto isClaimed = [&](uint64_t rva) {
    return llvm::is_contained(claimedThunkRVAs, rva);
  };

  auto canReuse = [&](uint64_t rva) {
    return rva != 0 && rva % thunkSize == 0 && rva >= tailCursor &&
           rva >= poolCursor && rva <= poolEndRVA &&
           poolEndRVA - rva >= thunkSize && !isClaimed(rva);
  };

  if (canReuse(oldPoolThunkRVA))
    return oldPoolThunkRVA;

  std::optional<uint64_t> bestFreedThunkRVA;
  for (uint64_t freedThunkRVA : freedThunkRVAs) {
    if (!canReuse(freedThunkRVA))
      continue;
    if (!bestFreedThunkRVA || freedThunkRVA > *bestFreedThunkRVA)
      bestFreedThunkRVA = freedThunkRVA;
  }
  if (bestFreedThunkRVA)
    return bestFreedThunkRVA;

  uint64_t nextCursor = poolCursor;
  while (nextCursor > tailCursor && nextCursor - tailCursor >= thunkSize) {
    uint64_t candidate = (nextCursor - thunkSize) & ~(thunkSize - 1);
    if (candidate < tailCursor)
      break;
    if (candidate <= poolEndRVA && poolEndRVA - candidate >= thunkSize &&
        !isClaimed(candidate))
      return candidate;
    nextCursor = candidate;
  }
  return std::nullopt;
}

void planIncrementalTextThunkAssignments(
    MutableArrayRef<IncrementalTextThunkPlanState> plans, uint64_t tailCursor,
    uint64_t &poolCursor, uint64_t &poolStart, uint64_t poolEndRVA,
    bool allowPoolThunks) {
  SmallVector<uint64_t, 8> claimedThunkRVAs;
  SmallVector<uint64_t, 8> freedThunkRVAs;
  auto releasePoolThunkRVA = [&](IncrementalTextThunkPlanState &plan) {
    if (plan.poolThunkRVA != 0 &&
        !llvm::is_contained(claimedThunkRVAs, plan.poolThunkRVA))
      freedThunkRVAs.push_back(plan.poolThunkRVA);
    plan.poolThunkRVA = 0;
    plan.usedPool = false;
  };

  for (IncrementalTextThunkPlanState &plan : plans) {
    if (!plan.active || plan.bodyRVA == 0 || plan.bodyRVA == plan.redirectRVA)
      continue;

    bool needPool = !isIncrementalAmd64Rel32InRange(
        llvm::COFF::IMAGE_REL_AMD64_REL32, plan.redirectRVA + 1, plan.bodyRVA);
    if (!needPool) {
      releasePoolThunkRVA(plan);
      continue;
    }

    if (!allowPoolThunks) {
      plan.active = false;
      releasePoolThunkRVA(plan);
      continue;
    }

    std::optional<uint64_t> poolThunkRVA =
        chooseIncrementalTextThunkRVA(plan.poolThunkRVA, tailCursor, poolCursor,
                                      poolEndRVA, claimedThunkRVAs,
                                      freedThunkRVAs);
    if (!poolThunkRVA) {
      plan.active = false;
      releasePoolThunkRVA(plan);
      continue;
    }

    if (poolStart == 0 || *poolThunkRVA < poolStart)
      poolStart = *poolThunkRVA;
    poolCursor = std::min(poolCursor, *poolThunkRVA);
    claimedThunkRVAs.push_back(*poolThunkRVA);
    plan.poolThunkRVA = *poolThunkRVA;
    plan.usedPool = true;
  }
}

bool isIncrementalAmd64Rel32InRange(uint16_t type, uint64_t sourceRVA,
                                    uint64_t targetRVA) {
  int64_t adjustment = 0;
  switch (type) {
  case llvm::COFF::IMAGE_REL_AMD64_REL32:
    adjustment = 4;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_1:
    adjustment = 5;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_2:
    adjustment = 6;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_3:
    adjustment = 7;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_4:
    adjustment = 8;
    break;
  case llvm::COFF::IMAGE_REL_AMD64_REL32_5:
    adjustment = 9;
    break;
  default:
    return true;
  }
  return isInt<32>(int64_t(targetRVA) - int64_t(sourceRVA) - adjustment);
}

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
  case IncrementalFallbackReason::MergeChunkParticipantChanged:
    return "MergeChunkParticipantChanged";
  case IncrementalFallbackReason::PackedSectionOverflow:
    return "PackedSectionOverflow";
  case IncrementalFallbackReason::Amd64Rel32OutOfRange:
    return "Amd64Rel32OutOfRange";
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
  session.currentArchiveOffsets.clear();
  session.inputIndices.clear();
  session.changedInputs.clear();
  session.reusedChunkData.clear();
  for (size_t i = 0; i < ctx.objFileInstances.size(); ++i) {
    ObjFile *file = ctx.objFileInstances[i];
    session.inputIndices[file] = i;
    session.currentInputHashes.push_back(xxh3_64bits(file->mb.getBuffer()));
    session.currentInputNames.push_back(file->getName().str());
    session.currentParentNames.push_back(file->archiveName.str());
    session.currentArchiveOffsets.push_back(file->archiveOffset);
  }
  return true;
}

IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk) {
  if (isa<IncrementalPaddingChunk>(&chunk))
    return IncrementalChunkKind::Padding;
  if (isa<IncrementalEntryRedirectChunkX64>(&chunk))
    return IncrementalChunkKind::EntryRedirect;
  if (isa<IncrementalLongThunkChunkX64>(&chunk))
    return IncrementalChunkKind::LongThunk;
  if (auto *section = dyn_cast<SectionChunk>(&chunk))
    if (section->file)
      return IncrementalChunkKind::ObjSection;
  return IncrementalChunkKind::Synthetic;
}

std::string getIncrementalChunkKey(const IncrementalLinkSession &session,
                                   const Chunk &chunk) {
  std::string key;
  raw_string_ostream os(key);
  if (auto *padding = dyn_cast<IncrementalPaddingChunk>(&chunk)) {
    os << "pad:" << padding->getSectionName() << ':' << chunk.getOutputCharacteristics()
       << ':' << chunk.getRVA() << ':' << chunk.getSize() << ':'
       << unsigned(padding->getFillByte());
    return os.str();
  }
  if (auto *redirect = dyn_cast<IncrementalEntryRedirectChunkX64>(&chunk)) {
    os << "redirect:" << redirect->getDebugName() << ':' << chunk.getSize();
    return os.str();
  }
  if (auto *thunk = dyn_cast<IncrementalLongThunkChunkX64>(&chunk)) {
    os << "longthunk:" << thunk->getDebugName();
    return os.str();
  }
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

static void disableIncrementalStateReuse(COFFLinkerContext &ctx) {
  if (!ctx.incrementalSession)
    return;
  ctx.incrementalSession->stateLoaded = false;
  ctx.incrementalSession->changedInputs.clear();
  ctx.incrementalSession->reusedChunkData.clear();
}

static bool validateIncrementalSymbolStates(COFFLinkerContext &ctx,
                                            IncrementalLinkSession &session) {
  std::vector<IncrementalSymbolState> currentSymbols =
      buildIncrementalSymbolStates(ctx, session);
  auto shouldCompareStrictly = [&](const IncrementalSymbolState &state) {
    if (session.state.layoutMode != IncrementalLayoutMode::Slotted)
      return true;
    return state.kind != IncrementalSymbolKind::Regular &&
           state.kind != IncrementalSymbolKind::Common;
  };

  SmallVector<const IncrementalSymbolState *, 32> filteredCurrentSymbols;
  SmallVector<const IncrementalSymbolState *, 32> filteredOldSymbols;
  for (const IncrementalSymbolState &symbol : currentSymbols)
    if (shouldCompareStrictly(symbol))
      filteredCurrentSymbols.push_back(&symbol);
  for (const IncrementalSymbolState &symbol : session.state.symbols)
    if (shouldCompareStrictly(symbol))
      filteredOldSymbols.push_back(&symbol);

  if (filteredCurrentSymbols.size() != filteredOldSymbols.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "resolved symbol count changed");
    disableIncrementalStateReuse(ctx);
    return false;
  }

  for (size_t i = 0; i < filteredCurrentSymbols.size(); ++i) {
    const IncrementalSymbolState &current = *filteredCurrentSymbols[i];
    const IncrementalSymbolState &old = *filteredOldSymbols[i];
    if (current.name != old.name || current.kind != old.kind ||
        current.inputIndex != old.inputIndex || current.value != old.value ||
        current.auxiliaryKey != old.auxiliaryKey) {
      std::string detail;
      raw_string_ostream os(detail);
      os << "symbol winner changed: " << current.name;
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             os.str());
      disableIncrementalStateReuse(ctx);
      return false;
    }
  }
  return true;
}

void prepareIncrementalLink(COFFLinkerContext &ctx) {
  ctx.config.incrementalLinkActive = false;
  ctx.config.incrementalLinkEligible = false;
  ctx.config.incrementalFallbackReason = IncrementalFallbackReason::None;
  ctx.config.incrementalFallbackDetail.clear();
  ctx.incrementalSession.reset();

  if (!ctx.config.incrementalLinkRequested ||
      !ctx.config.incrementalLinkSpecified)
    return;

  ensureIncrementalStatePath(ctx.config);
  auto session = std::make_unique<IncrementalLinkSession>();
  session->canWriteState = true;

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
  if (stateOrErr->outputPath != ctx.config.outputFile) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::OutputMismatch,
                           "incremental state was written for a different output");
    return;
  }
  if (stateOrErr->version < 3 ||
      stateOrErr->layoutMode != IncrementalLayoutMode::Slotted) {
    ctx.incrementalSession = std::move(session);
    setIncrementalFallback(ctx, IncrementalFallbackReason::MissingState,
                           "phase2 slotted baseline state is missing");
    return;
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
  session->softConfigChanged = session->state.softConfigHash != softHash;
  for (ArchiveFile *file : ctx.archiveFileInstances)
    session->replayableArchives.insert(file->getName());

  StringMap<ArchiveFile *> archives;
  for (ArchiveFile *file : ctx.archiveFileInstances)
    archives[file->getName()] = file;

  for (const IncrementalInputState &input : session->state.inputs) {
    if (input.parentName.empty())
      continue;
    auto archiveIt = archives.find(input.parentName);
    if (archiveIt == archives.end())
      continue;

    session->expectedArchiveMembers.insert(getIncrementalArchiveMemberKey(
        input.parentName, input.archiveOffset, input.name));
    if (input.archiveOffset != 0)
      archiveIt->second->addMemberByOffset(input.archiveOffset, input.name);
    else
      archiveIt->second->addMemberByName(input.name, input.name);
  }

  ctx.incrementalSession = std::move(session);
}

void finalizeIncrementalLinkPlan(COFFLinkerContext &ctx) {
  if (!ctx.incrementalSession || !ctx.incrementalSession->stateLoaded)
    return;

  IncrementalLinkSession &session = *ctx.incrementalSession;

  if (hasBitcodeInputs(ctx)) {
    session.canWriteState = false;
    setIncrementalFallback(ctx, IncrementalFallbackReason::LtoInput);
    disableIncrementalStateReuse(ctx);
    return;
  }

  uint64_t resourceHash = computeIncrementalResourceInputHash(ctx);
  if (resourceHash != session.state.resourceInputHash) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "resource or manifest inputs changed");
    disableIncrementalStateReuse(ctx);
    return;
  }

  prepareCurrentIncrementalInputs(ctx, session);

  if (session.state.inputs.size() != session.currentInputHashes.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "input count changed");
    disableIncrementalStateReuse(ctx);
    return;
  }

  for (size_t i = 0; i < session.state.inputs.size(); ++i) {
    const IncrementalInputState &input = session.state.inputs[i];
    if (input.name != session.currentInputNames[i] ||
        input.parentName != session.currentParentNames[i] ||
        input.archiveOffset != session.currentArchiveOffsets[i]) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "input order changed");
      disableIncrementalStateReuse(ctx);
      return;
    }
    if (input.contentHash != session.currentInputHashes[i])
      session.changedInputs.insert(ctx.objFileInstances[i]);
  }

  if (session.loadedArchiveMembers.size() != session.expectedArchiveMembers.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "archive member extraction changed");
    disableIncrementalStateReuse(ctx);
    return;
  }
  for (const auto &entry : session.expectedArchiveMembers) {
    if (!session.loadedArchiveMembers.contains(entry.getKey())) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "archive member extraction changed");
      disableIncrementalStateReuse(ctx);
      return;
    }
  }

  if (session.state.importTopologyHash != computeIncrementalImportTopologyHash(ctx)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "import topology changed");
    disableIncrementalStateReuse(ctx);
    return;
  }
  if (session.state.exportTopologyHash != computeIncrementalExportTopologyHash(ctx)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "export topology changed");
    disableIncrementalStateReuse(ctx);
    return;
  }

  if (!validateIncrementalSymbolStates(ctx, session))
    return;

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: using state " << ctx.config.incrementalStatePath;
    if (session.softConfigChanged)
      Log(ctx) << "incremental: soft-config metadata changed; rebuilding PDB metadata";
  }
}

void finalizeIncrementalLink(COFFLinkerContext &ctx) {
  if (errorCount() != 0 || !ctx.incrementalSession ||
      !ctx.incrementalSession->canWriteState)
    return;

  prepareCurrentIncrementalInputs(ctx, *ctx.incrementalSession);
  IncrementalStateFile state =
      buildIncrementalState(ctx, *ctx.incrementalSession);
  if (!ctx.incrementalSession->canWriteState)
    return;
  if (Error err = writeIncrementalState(ctx.config.incrementalStatePath, state))
    Warn(ctx) << "failed to write incremental state: " << toString(std::move(err));
}

void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      StringRef archiveName,
                                      uint64_t archiveOffset,
                                      StringRef memberName) {
  if (!ctx.incrementalSession || archiveName.empty())
    return;

  IncrementalLinkSession &session = *ctx.incrementalSession;
  if (!session.replayableArchives.contains(archiveName))
    return;
  session.loadedArchiveMembers.insert(
      getIncrementalArchiveMemberKey(archiveName, archiveOffset, memberName));
}

} // namespace lld::coff
