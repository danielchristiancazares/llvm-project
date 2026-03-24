#include "IncrementalLayout.h"
#include "COFFLinkerContext.h"
#include "Incremental.h"
#include "InputFiles.h"
#include "Symbols.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>

using namespace llvm;

namespace lld::coff {

namespace {

struct FreeRange {
  uint64_t startRVA = 0;
  uint64_t size = 0;
  uint8_t fillByte = 0;
};

struct PlannedSlot {
  IncrementalSlotRecordState slot;
  bool candidateFree = false;
  bool used = false;
};

struct PlannedChunk {
  uint64_t startRVA = 0;
  Chunk *chunk = nullptr;
};

static void clearReuseState(COFFLinkerContext &ctx) {
  if (!ctx.incrementalSession)
    return;
  ctx.incrementalSession->reusedChunkData.clear();
  ctx.incrementalSession->placementKinds.clear();
  ctx.incrementalSession->rewrittenChunks.clear();
}

static SmallVector<OutputSection *, 16>
getActiveSections(COFFLinkerContext &ctx) {
  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections) {
    bool preserved = false;
    if (ctx.incrementalSession && ctx.incrementalSession->stateLoaded) {
      for (const IncrementalSectionState &oldSection :
           ctx.incrementalSession->state.sections) {
        if (section->name == oldSection.name &&
            section->header.Characteristics == oldSection.characteristics) {
          preserved = true;
          break;
        }
      }
    }
    if (section->getVirtualSize() != 0 || preserved)
      activeSections.push_back(section);
  }
  return activeSections;
}

static bool hasDuplicateIncrementalChunkKeys(IncrementalLinkSession &session,
                                             const OutputSection &section) {
  StringSet<> keys;
  for (Chunk *chunk : section.chunks) {
    if (chunk->getSize() == 0)
      continue;
    std::string key = getIncrementalChunkKey(session, *chunk);
    if (!keys.insert(key).second)
      return true;
  }
  return false;
}

static const IncrementalSectionEnvelopeState *
findSectionEnvelope(const IncrementalStateFile &state, StringRef name,
                    uint32_t characteristics, uint32_t &envelopeIndex) {
  for (size_t i = 0; i < state.sectionEnvelopes.size(); ++i) {
    const IncrementalSectionEnvelopeState &envelope = state.sectionEnvelopes[i];
    if (envelope.name == name && envelope.characteristics == characteristics) {
      envelopeIndex = i;
      return &envelope;
    }
  }
  return nullptr;
}

static uint64_t getMinFragmentSize(IncrementalSlotClass slotClass) {
  switch (slotClass) {
  case IncrementalSlotClass::Text:
    return 16;
  case IncrementalSlotClass::RData:
  case IncrementalSlotClass::Data:
    return 8;
  case IncrementalSlotClass::None:
  case IncrementalSlotClass::PDataPacked:
  case IncrementalSlotClass::XDataPacked:
    return 0;
  }
  llvm_unreachable("unknown incremental slot class");
}

static std::string formatPlacementLog(StringRef action, StringRef sectionName,
                                      uint64_t rva, uint64_t size) {
  std::string message;
  raw_string_ostream os(message);
  os << "incremental: " << action << ' ' << sectionName << " RVA=0x"
     << utohexstr(rva) << " size=0x" << utohexstr(size);
  return os.str();
}

static bool validateAmd64Rel32Layout(COFFLinkerContext &ctx,
                                     const OutputSection &section) {
  for (Chunk *chunk : section.chunks) {
    auto *sectionChunk = dyn_cast<SectionChunk>(chunk);
    if (!sectionChunk || sectionChunk->getMachine() != AMD64)
      continue;

    for (const coff_relocation &rel : sectionChunk->getRelocs()) {
      int64_t p = int64_t(sectionChunk->getRVA()) + rel.VirtualAddress;
      auto *sym = dyn_cast_or_null<Defined>(
          sectionChunk->file->getSymbol(rel.SymbolTableIndex));
      if (!sym)
        continue;
      if (isIncrementalAmd64Rel32InRange(rel.Type, p, sym->getRVA()))
        continue;

      std::string detail;
      raw_string_ostream os(detail);
      os << "REL32 target out of range in .text for " << sym->getName();
      setIncrementalFallback(ctx,
                             IncrementalFallbackReason::Amd64Rel32OutOfRange,
                             os.str());
      return false;
    }
  }
  return true;
}

static bool applyExactSectionLayout(COFFLinkerContext &ctx,
                                    IncrementalLinkSession &session,
                                    OutputSection &currentSection,
                                    const IncrementalSectionState &oldSection,
                                    uint32_t sectionIndex) {
  if (currentSection.chunks.size() != oldSection.chunkCount) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "section layout changed: " + currentSection.name);
    return false;
  }

  if (oldSection.firstChunk > session.state.chunks.size() ||
      session.state.chunks.size() - oldSection.firstChunk < oldSection.chunkCount) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "chunk table range is invalid");
    return false;
  }

  currentSection.header.VirtualAddress = oldSection.rva;
  currentSection.header.VirtualSize = oldSection.virtualSize;
  currentSection.header.SizeOfRawData = oldSection.rawSize;

  for (size_t chunkOffset = 0; chunkOffset < currentSection.chunks.size();
       ++chunkOffset) {
    Chunk *currentChunk = currentSection.chunks[chunkOffset];
    const IncrementalChunkState &oldChunk =
        session.state.chunks[oldSection.firstChunk + chunkOffset];

    if (oldChunk.sectionIndex != sectionIndex ||
        classifyIncrementalChunk(*currentChunk) != oldChunk.kind ||
        getIncrementalChunkKey(session, *currentChunk) != oldChunk.key) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "chunk key mismatch in section " +
                                 currentSection.name);
      return false;
    }

    if (currentChunk->getSize() > oldChunk.slotCapacity) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::SlotOverflow,
                             "chunk grew past prior slot");
      return false;
    }

    currentChunk->setRVA(oldChunk.rva);

    auto *sectionChunk = dyn_cast<SectionChunk>(currentChunk);
    if (!sectionChunk || oldChunk.kind != IncrementalChunkKind::ObjSection)
      continue;

    auto fileIt = session.inputIndices.find(sectionChunk->file);
    if (fileIt == session.inputIndices.end() ||
        fileIt->second != oldChunk.inputIndex ||
        oldChunk.inputIndex >= session.state.inputs.size()) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "input index mismatch for section chunk");
      return false;
    }

    if (session.changedInputs.contains(sectionChunk->file) &&
        computeIncrementalSymbolHash(*sectionChunk) != oldChunk.symbolHash) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "symbol layout changed inside section chunk");
      return false;
    }
  }

  return true;
}

static bool planSlotReuseSection(COFFLinkerContext &ctx,
                                 IncrementalLinkSession &session,
                                 OutputSection &currentSection,
                                 bool &exactLayoutOnly,
                                 SmallVectorImpl<std::string> &verboseLogs) {
  uint32_t envelopeIndex = UINT32_MAX;
  const IncrementalSectionEnvelopeState *envelope = findSectionEnvelope(
      session.state, currentSection.name, currentSection.header.Characteristics,
      envelopeIndex);
  if (!envelope || !envelope->slotReuseEnabled ||
      !isIncrementalSlotReuseClass(envelope->slotClass)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "missing slot envelope in incremental state");
    return false;
  }

  StringSet<> currentKeys;
  SmallVector<std::pair<std::string, Chunk *>, 16> currentEntries;
  SmallVector<std::pair<std::string, Chunk *>, 4> zeroSizedEntries;
  currentEntries.reserve(currentSection.chunks.size());
  for (Chunk *chunk : currentSection.chunks) {
    std::string key = getIncrementalChunkKey(session, *chunk);
    if (chunk->getSize() == 0) {
      zeroSizedEntries.emplace_back(std::move(key), chunk);
      continue;
    }
    if (!currentKeys.insert(key).second) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "duplicate chunk key in .text");
      return false;
    }
    currentEntries.emplace_back(std::move(key), chunk);
  }

  StringMap<const IncrementalPlacementState *> placementsByKey;
  for (const IncrementalPlacementState &placement : session.state.placements) {
    if (placement.envelopeIndex != envelopeIndex)
      continue;
    placementsByKey[placement.key] = &placement;
  }

  SmallVector<PlannedSlot, 16> slots;
  for (const IncrementalSlotRecordState &slot : session.state.slotRecords) {
    if (slot.envelopeIndex != envelopeIndex)
      continue;
    PlannedSlot plannedSlot;
    plannedSlot.slot = slot;
    plannedSlot.candidateFree =
        slot.state == IncrementalSlotState::Free ||
        !currentKeys.contains(slot.occupantKey);
    slots.push_back(std::move(plannedSlot));
  }
  llvm::sort(slots, [](const PlannedSlot &lhs, const PlannedSlot &rhs) {
    return lhs.slot.startRVA < rhs.slot.startRVA;
  });

  DenseMap<uint64_t, size_t> slotByStart;
  for (size_t i = 0; i < slots.size(); ++i)
    slotByStart[slots[i].slot.startRVA] = i;

  SmallVector<PlannedChunk, 16> plannedChunks;
  SmallVector<FreeRange, 16> splitFreeRanges;
  uint64_t tailCursor = envelope->activeEndRVA;
  uint8_t fillByte = getIncrementalFillByte(envelope->slotClass);

  for (const auto &[key, chunk] : currentEntries) {
    uint64_t size = chunk->getSize();
    uint32_t alignment = chunk->getAlignment();

    if (auto placementIt = placementsByKey.find(key);
        placementIt != placementsByKey.end()) {
      auto slotIt = slotByStart.find(placementIt->second->startRVA);
      if (slotIt == slotByStart.end()) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                               "placement table references a missing slot");
        return false;
      }

      PlannedSlot &oldSlot = slots[slotIt->second];
      if (!oldSlot.used && size <= oldSlot.slot.capacity) {
        oldSlot.used = true;
        plannedChunks.push_back({placementIt->second->startRVA, chunk});
        continue;
      }

      oldSlot.candidateFree = true;
    }

    size_t bestSlotIndex = UINT32_MAX;
    for (size_t i = 0; i < slots.size(); ++i) {
      PlannedSlot &slot = slots[i];
      if (!slot.candidateFree || slot.used)
        continue;
      if (slot.slot.capacity < size)
        continue;
      if (slot.slot.startRVA % alignment != 0)
        continue;

      if (bestSlotIndex == UINT32_MAX ||
          slot.slot.capacity < slots[bestSlotIndex].slot.capacity ||
          (slot.slot.capacity == slots[bestSlotIndex].slot.capacity &&
           slot.slot.startRVA < slots[bestSlotIndex].slot.startRVA))
        bestSlotIndex = i;
    }

    if (bestSlotIndex != UINT32_MAX) {
      PlannedSlot &slot = slots[bestSlotIndex];
      slot.used = true;
      plannedChunks.push_back({slot.slot.startRVA, chunk});
      exactLayoutOnly = false;
      verboseLogs.push_back(formatPlacementLog("reused free slot",
                                               currentSection.name,
                                               slot.slot.startRVA, size));

      uint64_t remaining = slot.slot.capacity - size;
      if (remaining >= getMinFragmentSize(envelope->slotClass))
        splitFreeRanges.push_back(
            {slot.slot.startRVA + size, remaining, slot.slot.fillByte});
      continue;
    }

    uint64_t startRVA = alignTo(tailCursor, uint64_t(alignment));
    if (startRVA > envelope->maxSectionEndRVA ||
        envelope->maxSectionEndRVA - startRVA < size) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::SlotOverflow,
                             "chunk grew past preserved section envelope");
      return false;
    }

    if (startRVA > tailCursor)
      splitFreeRanges.push_back({tailCursor, startRVA - tailCursor, fillByte});
    plannedChunks.push_back({startRVA, chunk});
    tailCursor = startRVA + size;
    exactLayoutOnly = false;
    verboseLogs.push_back(formatPlacementLog("allocated tail reserve",
                                             currentSection.name, startRVA,
                                             size));
  }

  auto appendPadding = [&](uint64_t startRVA, uint64_t size, uint8_t fillByte) {
    if (size == 0)
      return;
    auto *padding = make<IncrementalPaddingChunk>(
        currentSection.name, currentSection.header.Characteristics,
        static_cast<uint32_t>(size), fillByte);
    plannedChunks.push_back({startRVA, padding});
  };

  for (const PlannedSlot &slot : slots) {
    if (!slot.candidateFree || slot.used)
      continue;
    appendPadding(slot.slot.startRVA, slot.slot.capacity, slot.slot.fillByte);
    if (slot.slot.state != IncrementalSlotState::Free)
      exactLayoutOnly = false;
  }
  for (const FreeRange &range : splitFreeRanges)
    appendPadding(range.startRVA, range.size, range.fillByte);

  for (const auto &[key, chunk] : zeroSizedEntries) {
    uint64_t startRVA = envelope->sectionRVA;
    if (auto placementIt = placementsByKey.find(key);
        placementIt != placementsByKey.end())
      startRVA = placementIt->second->startRVA;
    plannedChunks.push_back({startRVA, chunk});
  }

  if (envelope->slotClass == IncrementalSlotClass::Text) {
    SmallVector<std::pair<Chunk *, uint64_t>, 16> originalChunkRVAs;
    originalChunkRVAs.reserve(currentSection.chunks.size());
    for (Chunk *chunk : currentSection.chunks)
      originalChunkRVAs.emplace_back(chunk, chunk->getRVA());

    for (const PlannedChunk &planned : plannedChunks)
      planned.chunk->setRVA(planned.startRVA);
    if (!validateAmd64Rel32Layout(ctx, currentSection)) {
      for (const auto &[chunk, rva] : originalChunkRVAs)
        chunk->setRVA(rva);
      return false;
    }
  }

  llvm::sort(plannedChunks, [](const PlannedChunk &lhs, const PlannedChunk &rhs) {
    if (lhs.startRVA != rhs.startRVA)
      return lhs.startRVA < rhs.startRVA;
    return lhs.chunk->getSize() < rhs.chunk->getSize();
  });

  currentSection.chunks.clear();
  currentSection.header.VirtualAddress = envelope->sectionRVA;
  uint64_t activeEnd = envelope->sectionRVA;
  for (const PlannedChunk &planned : plannedChunks) {
    planned.chunk->setRVA(planned.startRVA);
    currentSection.chunks.push_back(planned.chunk);
    activeEnd = std::max(activeEnd, planned.startRVA + planned.chunk->getSize());
  }
  currentSection.header.VirtualSize = activeEnd - envelope->sectionRVA;
  currentSection.header.SizeOfRawData =
      alignTo(currentSection.header.VirtualSize, uint64_t(ctx.config.fileAlign));
  return true;
}

static bool planPackedSection(COFFLinkerContext &ctx,
                              IncrementalLinkSession &session,
                              OutputSection &currentSection,
                              bool &exactLayoutOnly) {
  uint32_t envelopeIndex = UINT32_MAX;
  const IncrementalSectionEnvelopeState *envelope = findSectionEnvelope(
      session.state, currentSection.name, currentSection.header.Characteristics,
      envelopeIndex);
  if (!envelope || !envelope->packedActivePrefix ||
      !isIncrementalPackedClass(envelope->slotClass)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "missing packed-section envelope in incremental state");
    return false;
  }

  StringMap<const IncrementalPlacementState *> placementsByKey;
  for (const IncrementalPlacementState &placement : session.state.placements) {
    if (placement.envelopeIndex != envelopeIndex)
      continue;
    placementsByKey[placement.key] = &placement;
  }

  currentSection.header.VirtualAddress = envelope->sectionRVA;
  uint64_t cursor = envelope->sectionRVA;
  for (Chunk *chunk : currentSection.chunks) {
    if (chunk->getSize() == 0) {
      chunk->setRVA(cursor);
      continue;
    }

    std::string key = getIncrementalChunkKey(session, *chunk);
    uint64_t startRVA = alignTo(cursor, uint64_t(chunk->getAlignment()));
    if (startRVA > envelope->maxSectionEndRVA ||
        envelope->maxSectionEndRVA - startRVA < chunk->getSize()) {
      setIncrementalFallback(ctx,
                             IncrementalFallbackReason::PackedSectionOverflow,
                             "packed section exceeded preserved envelope");
      return false;
    }

    chunk->setRVA(startRVA);
    cursor = startRVA + chunk->getSize();
    if (auto placementIt = placementsByKey.find(key);
        placementIt == placementsByKey.end() ||
        placementIt->second->startRVA != startRVA)
      exactLayoutOnly = false;
  }

  currentSection.header.VirtualSize = cursor - envelope->sectionRVA;
  currentSection.header.SizeOfRawData =
      alignTo(currentSection.header.VirtualSize, uint64_t(ctx.config.fileAlign));
  return true;
}

static void recomputeOutputLayout(COFFLinkerContext &ctx,
                                  ArrayRef<OutputSection *> activeSections,
                                  IncrementalLayoutResult &result) {
  result.sizeOfHeaders = ctx.incrementalSession->state.sizeOfHeaders;
  uint64_t fileSize = result.sizeOfHeaders;
  uint64_t imageEnd = result.sizeOfHeaders;
  for (OutputSection *section : activeSections) {
    if (section->getRawSize() != 0)
      section->header.PointerToRawData = fileSize;
    else
      section->header.PointerToRawData = 0;
    fileSize += alignTo(section->getRawSize(), uint64_t(ctx.config.fileAlign));
    imageEnd = std::max(imageEnd, section->getRVA() +
                                      alignTo(section->getVirtualSize(),
                                              uint64_t(ctx.config.align)));
  }
  result.fileSize = fileSize;
  result.sizeOfImage = alignTo(imageEnd, uint64_t(ctx.config.align));
}

} // namespace

bool applyIncrementalLayout(COFFLinkerContext &ctx,
                            IncrementalLayoutResult &result) {
  if (!ctx.incrementalSession || !ctx.incrementalSession->stateLoaded)
    return false;

  IncrementalLinkSession &session = *ctx.incrementalSession;
  clearReuseState(ctx);

  SmallVector<OutputSection *, 16> activeSections = getActiveSections(ctx);
  if (activeSections.size() != session.state.sections.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "output section count changed");
    return false;
  }

  bool exactLayoutOnly = true;
  SmallVector<std::string, 8> verboseLogs;
  for (size_t sectionIndex = 0; sectionIndex < activeSections.size();
       ++sectionIndex) {
    OutputSection *currentSection = activeSections[sectionIndex];
    const IncrementalSectionState &oldSection =
        session.state.sections[sectionIndex];

    if (currentSection->name != oldSection.name ||
        currentSection->header.Characteristics != oldSection.characteristics) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "section layout changed: " + currentSection->name);
      return false;
    }

    IncrementalSlotClass slotClass = classifyIncrementalSection(
        currentSection->name, currentSection->header.Characteristics);
    if (isIncrementalSlotReuseClass(slotClass) &&
        !hasDuplicateIncrementalChunkKeys(session, *currentSection) &&
        !(slotClass == IncrementalSlotClass::RData &&
          llvm::any_of(currentSection->chunks,
                       [](const Chunk *chunk) { return isa<MergeChunk>(chunk); }))) {
      if (!planSlotReuseSection(ctx, session, *currentSection, exactLayoutOnly,
                                verboseLogs))
        return false;
      continue;
    }
    if (isIncrementalPackedClass(slotClass)) {
      if (!planPackedSection(ctx, session, *currentSection, exactLayoutOnly))
        return false;
      continue;
    }

    if (!applyExactSectionLayout(ctx, session, *currentSection, oldSection,
                                 sectionIndex))
      return false;
  }

  recomputeOutputLayout(ctx, activeSections, result);
  ctx.config.incrementalLinkActive = true;
  ctx.config.incrementalFallbackReason = IncrementalFallbackReason::None;
  ctx.config.incrementalFallbackDetail.clear();

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: phase2 active";
    if (exactLayoutOnly)
      Log(ctx) << "incremental: exact-layout reuse active";
    for (const std::string &log : verboseLogs)
      Log(ctx) << log;
  }
  return true;
}

} // namespace lld::coff
