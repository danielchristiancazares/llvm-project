#include "IncrementalLayout.h"
#include "COFFLinkerContext.h"
#include "Incremental.h"
#include "IncrementalRedirects.h"
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
  enum class Availability : uint8_t {
    Blocked = 1,
    Available = 2,
    ReservedForRedirect = 3,
    Claimed = 4,
  };

  Availability availability = Availability::Blocked;
};

struct PlannedChunk {
  uint64_t startRVA = 0;
  Chunk *chunk = nullptr;
};

struct RedirectPlanEntry {
  std::string targetKey;
  std::string canonicalSymbol;
  uint64_t redirectRVA = 0;
  uint64_t redirectCapacity = 0;
  uint64_t bodyRVA = 0;
  uint64_t poolThunkRVA = 0;
  uint32_t minAlignment = 1;
  IncrementalRedirectEngagement engagement =
      IncrementalRedirectEngagement::Deferred;
  IncrementalRedirectProvenance provenance =
      IncrementalRedirectProvenance::FreshInstall;
  IncrementalRedirectTargeting targeting =
      IncrementalRedirectTargeting::DirectBodyTarget;
  Defined *bodyTarget = nullptr;
};

static bool hasInstalledRedirect(const RedirectPlanEntry &plan) {
  return plan.engagement == IncrementalRedirectEngagement::Installed;
}

static bool hadLegacyRedirect(const RedirectPlanEntry &plan) {
  return plan.provenance == IncrementalRedirectProvenance::LegacyRedirect;
}

static bool usesPoolThunk(const RedirectPlanEntry &plan) {
  return plan.targeting == IncrementalRedirectTargeting::PoolThunkTarget;
}

static std::unique_ptr<LayoutStableLink>
takeActiveLayoutStableLink(COFFLinkerContext &ctx) {
  std::unique_ptr<LayoutStableLink> result;
  ctx.incremental->match(
      [&](IncrementalDisabled &) {},
      [&](FullImageBuild &) {},
      [&](StateBackedLink &) {},
      [&](LayoutStableLink &validated) {
        result = std::make_unique<LayoutStableLink>(std::move(validated));
      },
      [&](ByteReuseLink &) {});
  if (result)
    ctx.incremental = IncrementalCoordinator::makeDisabled();
  return result;
}

static void clearReuseState(IncrementalReuseData &reuse) {
  reuse.reusedChunkData.clear();
  reuse.rewrittenChunks.clear();
  reuse.currentTextRedirects.clear();
  reuse.currentTextThunkPool = {};
  reuse.redirectSymbols.clear();
  reuse.poolThunkSymbols.clear();
  reuse.movedChunkTargets.clear();
  reuse.activeRedirectTargets.clear();
  reuse.currentEdges.clear();
}

static SmallVector<OutputSection *, 16>
getActiveSections(COFFLinkerContext &ctx,
                  const IncrementalStateFile *loadedState) {
  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections) {
    bool preserved = false;
    if (loadedState &&
        loadedState->layoutMode == IncrementalLayoutMode::Slotted) {
      for (const IncrementalSectionState &oldSection : loadedState->sections) {
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

static bool hasDuplicateIncrementalChunkKeys(
    const IncrementalInputIndexMap &inputIndices,
                                             const OutputSection &section) {
  StringSet<> keys;
  for (Chunk *chunk : section.chunks) {
    if (chunk->getSize() == 0)
      continue;
    std::string key = getIncrementalChunkKey(inputIndices, *chunk);
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

static uint64_t
getMinFragmentSize(IncrementalSectionLayoutKind layoutKind) {
  switch (layoutKind) {
  case IncrementalSectionLayoutKind::TextFreeSlots:
    return 16;
  case IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots:
  case IncrementalSectionLayoutKind::WritableDataFreeSlots:
    return 8;
  case IncrementalSectionLayoutKind::ExactSectionLayout:
  case IncrementalSectionLayoutKind::PackedPDataPrefix:
  case IncrementalSectionLayoutKind::PackedXDataPrefix:
    return 0;
  }
  llvm_unreachable("unknown incremental section layout");
}

static bool validateSlotReuseState(COFFLinkerContext &ctx,
                                   const IncrementalSectionEnvelopeState &envelope,
                                   uint32_t envelopeIndex,
                                   ArrayRef<IncrementalSlotRecordState> slotRecords) {
  if (envelope.sectionRVA > envelope.activeEndRVA ||
      envelope.activeEndRVA > envelope.maxSectionEndRVA ||
      envelope.maxSectionEndRVA - envelope.sectionRVA > UINT32_MAX) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "slot envelope contains an invalid preserved range");
    return false;
  }

  SmallVector<const IncrementalSlotRecordState *, 16> slots;
  for (const IncrementalSlotRecordState &slot : slotRecords) {
    if (slot.envelopeIndex != envelopeIndex)
      continue;

    uint64_t slotEnd = 0;
    if (slot.capacity == 0 || slot.capacity > UINT32_MAX ||
        slot.minAlignment == 0 || slot.startRVA < envelope.sectionRVA ||
        slot.startRVA >= envelope.activeEndRVA ||
        slot.startRVA > UINT64_MAX - slot.capacity) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "slot table contains invalid preserved range");
      return false;
    }
    slotEnd = slot.startRVA + slot.capacity;
    if (slotEnd > envelope.activeEndRVA || slot.committedSize > slot.capacity) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "slot table contains invalid preserved range");
      return false;
    }

    if (slot.state != IncrementalSlotState::Occupied &&
        slot.state != IncrementalSlotState::Free) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "slot table contains an invalid slot state");
      return false;
    }

    if (slot.state == IncrementalSlotState::Occupied) {
      if (slot.occupantKey.empty()) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                               "occupied slot is missing its preserved occupant");
        return false;
      }
      if (envelope.layoutKind == IncrementalSectionLayoutKind::TextFreeSlots &&
          StringRef(slot.occupantKey).starts_with("longthunk:")) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                               "slot table contains stale long thunk state");
        return false;
      }
    }

    slots.push_back(&slot);
  }

  llvm::sort(slots, [](const IncrementalSlotRecordState *lhs,
                       const IncrementalSlotRecordState *rhs) {
    return lhs->startRVA < rhs->startRVA;
  });

  uint64_t previousEnd = envelope.sectionRVA;
  for (const IncrementalSlotRecordState *slot : slots) {
    uint64_t slotEnd = slot->startRVA + slot->capacity;
    if (slot->startRVA < previousEnd) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "slot table contains overlapping preserved ranges");
      return false;
    }
    previousEnd = slotEnd;
  }

  return true;
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
                                     const DenseSet<const Chunk *> &rewrittenChunks,
                                     const OutputSection &section) {
  for (Chunk *chunk : section.chunks) {
    auto *sectionChunk = dyn_cast<SectionChunk>(chunk);
    if (!sectionChunk) {
      auto *nonSection = dyn_cast<NonSectionChunk>(chunk);
      if (nonSection && !nonSection->verifyRanges()) {
        setIncrementalFallback(ctx,
                               IncrementalFallbackReason::Amd64Rel32OutOfRange,
                               "incremental redirect target out of range");
        return false;
      }
      continue;
    }
    if (sectionChunk->getMachine() != AMD64)
      continue;
    if (!rewrittenChunks.contains(sectionChunk))
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

static void rewriteRelocsToRedirectTargets(COFFLinkerContext &ctx,
                                           const IncrementalInputIndexMap &inputIndices,
                                           IncrementalReuseData &reuse) {
  StringMap<SmallVector<const IncrementalEdgeState *, 4>> edgesBySource;
  for (const IncrementalEdgeState &edge : reuse.currentEdges) {
    if (edge.routing !=
            IncrementalEdgeRouting::RedirectEligibleEntryReference ||
        !reuse.activeRedirectTargets.contains(edge.targetKey))
      continue;
    edgesBySource[edge.sourceKey].push_back(&edge);
  }

  DenseMap<std::pair<ObjFile *, Defined *>, uint32_t> thunkSymtabIndices;
  for (OutputSection *section : ctx.outputSections) {
    for (Chunk *chunk : section->chunks) {
      auto *source = dyn_cast<SectionChunk>(chunk);
      if (!source || !reuse.rewrittenChunks.contains(source))
        continue;

      std::string sourceKey = getIncrementalChunkKey(inputIndices, *source);
      auto edgesIt = edgesBySource.find(sourceKey);
      if (edgesIt == edgesBySource.end())
        continue;

      SmallVector<std::pair<uint32_t, uint32_t>, 4> relocReplacements;
      ArrayRef<coff_relocation> currentRelocs = source->getRelocs();
      for (size_t i = 0; i < currentRelocs.size(); ++i) {
        const coff_relocation &rel = currentRelocs[i];
        for (const IncrementalEdgeState *edge : edgesIt->second) {
          if (edge->sourceOffset != rel.VirtualAddress)
            continue;
          Defined *redirectSym = reuse.redirectSymbols.lookup(edge->targetKey);
          if (!redirectSym)
            continue;
          auto insertion =
              thunkSymtabIndices.insert({{source->file, redirectSym}, ~0U});
          uint32_t &symbolIndex = insertion.first->second;
          if (insertion.second)
            symbolIndex = source->file->addRangeThunkSymbol(redirectSym);
          relocReplacements.emplace_back(i, symbolIndex);
          break;
        }
      }
      if (relocReplacements.empty())
        continue;

      MutableArrayRef<coff_relocation> newRelocs;
      auto objectRelocs = source->file->getCOFFObj()->getRelocations(source->header);
      if (objectRelocs.data() == currentRelocs.data()) {
        newRelocs = MutableArrayRef(
            bAlloc().Allocate<coff_relocation>(currentRelocs.size()),
            currentRelocs.size());
      } else {
        newRelocs = MutableArrayRef(
            const_cast<coff_relocation *>(currentRelocs.data()), currentRelocs.size());
      }

      auto nextReplacement = relocReplacements.begin();
      auto endReplacement = relocReplacements.end();
      for (size_t i = 0; i < currentRelocs.size(); ++i) {
        newRelocs[i] = currentRelocs[i];
        if (nextReplacement != endReplacement && nextReplacement->first == i) {
          newRelocs[i].SymbolTableIndex = nextReplacement->second;
          ++nextReplacement;
        }
      }
      source->setRelocs(newRelocs);
    }
  }
}

static bool applyExactSectionLayout(COFFLinkerContext &ctx,
                                    IncrementalBaselineData &baseline,
                                    OutputSection &currentSection,
                                    const IncrementalSectionState &oldSection,
                                    uint32_t sectionIndex) {
  if (currentSection.chunks.size() != oldSection.chunkCount) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "section layout changed: " + currentSection.name);
    return false;
  }

  if (oldSection.firstChunk > baseline.state.chunks.size() ||
      baseline.state.chunks.size() - oldSection.firstChunk <
          oldSection.chunkCount) {
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
        baseline.state.chunks[oldSection.firstChunk + chunkOffset];

    if (oldChunk.sectionIndex != sectionIndex ||
        classifyIncrementalChunk(*currentChunk) != oldChunk.kind ||
        getIncrementalChunkKey(baseline.currentInputs.inputIndices,
                               *currentChunk) != oldChunk.key) {
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

    auto fileIt = baseline.currentInputs.inputIndices.find(sectionChunk->file);
    if (fileIt == baseline.currentInputs.inputIndices.end() ||
        fileIt->second != oldChunk.inputIndex ||
        oldChunk.inputIndex >= baseline.state.inputs.size()) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "input index mismatch for section chunk");
      return false;
    }

    if (baseline.changedInputs.contains(sectionChunk->file) &&
        computeIncrementalSymbolHash(*sectionChunk) != oldChunk.symbolHash) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "symbol layout changed inside section chunk");
      return false;
    }
  }

  return true;
}

static bool planSlotReuseSection(COFFLinkerContext &ctx,
                                 IncrementalBaselineData &baseline,
                                 IncrementalReuseData &reuse,
                                 OutputSection &currentSection,
                                 bool &exactLayoutOnly,
                                 SmallVectorImpl<std::string> &verboseLogs) {
  uint32_t envelopeIndex = UINT32_MAX;
  const IncrementalSectionEnvelopeState *envelope = findSectionEnvelope(
      baseline.state, currentSection.name, currentSection.header.Characteristics,
      envelopeIndex);
  if (!envelope || !isIncrementalFreeSlotLayout(envelope->layoutKind)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "missing slot envelope in incremental state");
    return false;
  }

  StringSet<> currentKeys;
  SmallVector<std::pair<std::string, Chunk *>, 16> currentEntries;
  SmallVector<std::pair<std::string, Chunk *>, 4> zeroSizedEntries;
  currentEntries.reserve(currentSection.chunks.size());
  for (Chunk *chunk : currentSection.chunks) {
    std::string key =
        getIncrementalChunkKey(baseline.currentInputs.inputIndices, *chunk);
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
  for (const IncrementalPlacementState &placement : baseline.state.placements) {
    if (placement.envelopeIndex != envelopeIndex)
      continue;
    placementsByKey[placement.key] = &placement;
  }

  StringMap<const IncrementalTextRedirectState *> redirectsByKey;
  for (const IncrementalTextRedirectState &redirect :
       baseline.state.textRedirects)
    redirectsByKey[redirect.targetKey] = &redirect;

  SmallVector<PlannedSlot, 16> slots;
  for (const IncrementalSlotRecordState &slot : baseline.state.slotRecords) {
    if (slot.envelopeIndex != envelopeIndex)
      continue;
    PlannedSlot plannedSlot;
    plannedSlot.slot = slot;
    plannedSlot.availability =
        slot.state == IncrementalSlotState::Free ||
                !currentKeys.contains(slot.occupantKey)
            ? PlannedSlot::Availability::Available
            : PlannedSlot::Availability::Blocked;
    slots.push_back(std::move(plannedSlot));
  }
  llvm::sort(slots, [](const PlannedSlot &lhs, const PlannedSlot &rhs) {
    return lhs.slot.startRVA < rhs.slot.startRVA;
  });

  DenseMap<uint64_t, size_t> slotByStart;
  for (size_t i = 0; i < slots.size(); ++i)
    slotByStart[slots[i].slot.startRVA] = i;

  StringMap<RedirectPlanEntry> redirectPlans;
  SmallVector<RedirectPlanEntry *, 8> orderedRedirectPlans;
  if (envelope->layoutKind == IncrementalSectionLayoutKind::TextFreeSlots) {
    for (const auto &[key, chunk] : currentEntries) {
      auto *sectionChunk = dyn_cast<SectionChunk>(chunk);
      if (!sectionChunk || sectionChunk->getMachine() != AMD64)
        continue;

      Defined *canonicalSymbol = findIncrementalCanonicalEntrySymbol(*sectionChunk);
      if (!canonicalSymbol)
        continue;

      const IncrementalPlacementState *oldPlacement = nullptr;
      if (auto placementIt = placementsByKey.find(key);
          placementIt != placementsByKey.end())
        oldPlacement = placementIt->second;

      const IncrementalTextRedirectState *oldRedirect = nullptr;
      if (auto redirectIt = redirectsByKey.find(key);
          redirectIt != redirectsByKey.end())
        oldRedirect = redirectIt->second;

      uint64_t redirectRVA = 0;
      uint64_t redirectCapacity = 0;
      size_t slotIndex = size_t(-1);
      if (oldRedirect) {
        auto slotIt = slotByStart.find(oldRedirect->redirectRVA);
        if (slotIt == slotByStart.end()) {
          setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                                 "redirect table references a missing legacy slot");
          return false;
        }
        redirectRVA = oldRedirect->redirectRVA;
        redirectCapacity = oldRedirect->redirectCapacity;
        slotIndex = slotIt->second;
      } else if (oldPlacement) {
        auto slotIt = slotByStart.find(oldPlacement->startRVA);
        if (slotIt == slotByStart.end()) {
          setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                                 "placement table references a missing slot");
          return false;
        }
        const PlannedSlot &oldSlot = slots[slotIt->second];
        if (chunk->getSize() > oldSlot.slot.capacity && oldSlot.slot.capacity >= 5) {
          redirectRVA = oldPlacement->startRVA;
          redirectCapacity = oldSlot.slot.capacity;
          slotIndex = slotIt->second;
        }
      }

      if (slotIndex == size_t(-1))
        continue;

      PlannedSlot &redirectSlot = slots[slotIndex];
      redirectSlot.availability = PlannedSlot::Availability::ReservedForRedirect;

      RedirectPlanEntry plan;
      plan.targetKey = key;
      plan.canonicalSymbol = canonicalSymbol->getName().str();
      plan.redirectRVA = redirectRVA;
      plan.redirectCapacity = redirectCapacity;
      plan.minAlignment = redirectSlot.slot.minAlignment;
      plan.bodyTarget = canonicalSymbol;
      if (oldRedirect) {
        plan.engagement = IncrementalRedirectEngagement::Installed;
        plan.provenance = IncrementalRedirectProvenance::LegacyRedirect;
        plan.poolThunkRVA = oldRedirect->poolThunkRVA;
        if (oldRedirect->poolThunkRVA != 0)
          plan.targeting = IncrementalRedirectTargeting::PoolThunkTarget;
      }
      redirectPlans[key] = std::move(plan);
    }

    for (const auto &[key, chunk] : currentEntries) {
      (void)chunk;
      if (auto redirectIt = redirectPlans.find(key); redirectIt != redirectPlans.end())
        orderedRedirectPlans.push_back(&redirectIt->second);
    }
  }

  SmallVector<PlannedChunk, 16> plannedChunks;
  SmallVector<FreeRange, 16> splitFreeRanges;
  uint64_t tailCursor = envelope->activeEndRVA;
  uint8_t fillByte = getIncrementalFillByte(envelope->layoutKind);

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
      if (oldSlot.availability != PlannedSlot::Availability::Claimed &&
          size <= oldSlot.slot.capacity) {
        oldSlot.availability = PlannedSlot::Availability::Claimed;
        plannedChunks.push_back({placementIt->second->startRVA, chunk});
        if (auto redirectIt = redirectPlans.find(key); redirectIt != redirectPlans.end())
          redirectIt->second.bodyRVA = placementIt->second->startRVA;
        continue;
      }

      if (oldSlot.availability != PlannedSlot::Availability::ReservedForRedirect)
        oldSlot.availability = PlannedSlot::Availability::Available;
    }

    size_t bestSlotIndex = UINT32_MAX;
    for (size_t i = 0; i < slots.size(); ++i) {
      PlannedSlot &slot = slots[i];
      if (slot.availability != PlannedSlot::Availability::Available)
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
      slot.availability = PlannedSlot::Availability::Claimed;
      plannedChunks.push_back({slot.slot.startRVA, chunk});
      if (auto redirectIt = redirectPlans.find(key); redirectIt != redirectPlans.end()) {
        redirectIt->second.bodyRVA = slot.slot.startRVA;
        redirectIt->second.engagement = IncrementalRedirectEngagement::Installed;
      }
      if (auto placementIt = placementsByKey.find(key);
          placementIt != placementsByKey.end() &&
          placementIt->second->startRVA != slot.slot.startRVA)
        reuse.movedChunkTargets.insert(key);
      exactLayoutOnly = false;
      verboseLogs.push_back(formatPlacementLog("reused free slot",
                                               currentSection.name,
                                               slot.slot.startRVA, size));

      uint64_t remaining = slot.slot.capacity - size;
      if (remaining >= getMinFragmentSize(envelope->layoutKind))
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
    if (auto redirectIt = redirectPlans.find(key); redirectIt != redirectPlans.end()) {
      redirectIt->second.bodyRVA = startRVA;
      redirectIt->second.engagement = IncrementalRedirectEngagement::Installed;
    }
    if (placementsByKey.count(key))
      reuse.movedChunkTargets.insert(key);
    tailCursor = startRVA + size;
    exactLayoutOnly = false;
    verboseLogs.push_back(formatPlacementLog("allocated tail reserve",
                                             currentSection.name, startRVA,
                                             size));
  }

  if (envelope->layoutKind == IncrementalSectionLayoutKind::TextFreeSlots) {
    auto makeSyntheticName = [&](StringRef prefix, uint64_t rva) {
      std::string name;
      raw_string_ostream os(name);
      os << prefix << '$' << utohexstr(rva);
      return saver().save(os.str());
    };

    uint64_t poolCursor = baseline.state.textThunkPool.nextFreeRVA != 0
                              ? baseline.state.textThunkPool.nextFreeRVA
                              : envelope->maxSectionEndRVA;
    uint64_t poolStart = baseline.state.textThunkPool.poolStartRVA;
    uint64_t poolEnd = baseline.state.textThunkPool.poolEndRVA != 0
                           ? baseline.state.textThunkPool.poolEndRVA
                           : envelope->maxSectionEndRVA;
    SmallVector<IncrementalTextThunkPlanState, 8> thunkPlans;
    thunkPlans.reserve(orderedRedirectPlans.size());
    for (RedirectPlanEntry *plan : orderedRedirectPlans)
      thunkPlans.push_back({plan->redirectRVA, plan->bodyRVA, plan->poolThunkRVA,
                            plan->engagement, plan->provenance,
                            plan->targeting});
    planIncrementalTextThunkAssignments(thunkPlans, tailCursor, poolCursor,
                                        poolStart, poolEnd,
                                        !ctx.config.guardCF);

    for (size_t i = 0; i < orderedRedirectPlans.size(); ++i) {
      RedirectPlanEntry &plan = *orderedRedirectPlans[i];
      const IncrementalTextThunkPlanState &thunkPlan = thunkPlans[i];
      uint64_t oldPoolThunkRVA = plan.poolThunkRVA;
      plan.engagement = thunkPlan.engagement;
      plan.poolThunkRVA = thunkPlan.poolThunkRVA;
      plan.targeting = thunkPlan.targeting;

      if (!hasInstalledRedirect(plan)) {
        if (hadLegacyRedirect(plan)) {
          reuse.movedChunkTargets.insert(plan.targetKey);
          exactLayoutOnly = false;
        }
        continue;
      }

      if (plan.poolThunkRVA != oldPoolThunkRVA)
        exactLayoutOnly = false;

      if (usesPoolThunk(plan)) {
        auto *poolChunk = make<IncrementalLongThunkChunkX64>(
            saver().save("redirect-pool:" + plan.targetKey), plan.bodyTarget,
            ctx.config.imageBase);
        poolChunk->setRVA(plan.poolThunkRVA);
        auto *poolSymbol = make<DefinedSynthetic>(
            makeSyntheticName("__incremental_redirect_pool", plan.poolThunkRVA),
            poolChunk);
        plannedChunks.push_back({plan.poolThunkRVA, poolChunk});
        reuse.poolThunkSymbols[plan.targetKey] = poolSymbol;
        plan.bodyTarget = poolSymbol;
        verboseLogs.push_back(formatPlacementLog("allocated long thunk pool",
                                                 currentSection.name,
                                                 plan.poolThunkRVA,
                                                 poolChunk->getSize()));
      }

      auto *redirectChunk = make<IncrementalEntryRedirectChunkX64>(
          saver().save("entry-redirect:" + plan.targetKey), plan.bodyTarget,
          static_cast<uint32_t>(plan.redirectCapacity), plan.minAlignment);
      plannedChunks.push_back({plan.redirectRVA, redirectChunk});
      auto *redirectSymbol = make<DefinedSynthetic>(
          makeSyntheticName("__incremental_entry_redirect", plan.redirectRVA),
          redirectChunk);
      reuse.redirectSymbols[plan.targetKey] = redirectSymbol;

      IncrementalTextRedirectState redirectState;
      redirectState.targetKey = plan.targetKey;
      redirectState.canonicalSymbol = plan.canonicalSymbol;
      redirectState.redirectRVA = plan.redirectRVA;
      redirectState.redirectCapacity = plan.redirectCapacity;
      redirectState.bodyRVA = plan.bodyRVA;
      redirectState.poolThunkRVA = plan.poolThunkRVA;
      reuse.currentTextRedirects.push_back(std::move(redirectState));
      reuse.activeRedirectTargets.insert(plan.targetKey);
      verboseLogs.push_back(formatPlacementLog("installed legacy redirect",
                                               currentSection.name,
                                               plan.redirectRVA,
                                               plan.redirectCapacity));
      exactLayoutOnly = false;
    }

    if (!reuse.currentTextRedirects.empty()) {
      reuse.currentTextThunkPool.poolStartRVA = poolStart;
      reuse.currentTextThunkPool.poolEndRVA = poolEnd;
      reuse.currentTextThunkPool.nextFreeRVA = poolCursor;
    }
  }

  auto appendPadding = [&](uint64_t startRVA, uint64_t size,
                           uint8_t fillByte) -> bool {
    uint64_t endRVA = 0;
    if (size == 0)
      return true;
    if (size > UINT32_MAX || startRVA < envelope->sectionRVA ||
        startRVA > UINT64_MAX - size) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "padding range exceeds preserved section envelope");
      return false;
    }
    endRVA = startRVA + size;
    if (endRVA > envelope->maxSectionEndRVA) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "padding range exceeds preserved section envelope");
      return false;
    }
    auto *padding = make<IncrementalPaddingChunk>(
        currentSection.name, currentSection.header.Characteristics,
        static_cast<uint32_t>(size), fillByte);
    plannedChunks.push_back({startRVA, padding});
    return true;
  };

  for (const PlannedSlot &slot : slots) {
    if (slot.availability != PlannedSlot::Availability::Available)
      continue;
    if (!appendPadding(slot.slot.startRVA, slot.slot.capacity, slot.slot.fillByte))
      return false;
    if (slot.slot.state != IncrementalSlotState::Free)
      exactLayoutOnly = false;
  }
  for (const FreeRange &range : splitFreeRanges)
    if (!appendPadding(range.startRVA, range.size, range.fillByte))
      return false;

  for (const auto &[key, chunk] : zeroSizedEntries) {
    uint64_t startRVA = envelope->sectionRVA;
    if (auto placementIt = placementsByKey.find(key);
        placementIt != placementsByKey.end())
      startRVA = placementIt->second->startRVA;
    plannedChunks.push_back({startRVA, chunk});
  }

  llvm::sort(plannedChunks, [](const PlannedChunk &lhs, const PlannedChunk &rhs) {
    if (lhs.startRVA != rhs.startRVA)
      return lhs.startRVA < rhs.startRVA;
    return lhs.chunk->getSize() < rhs.chunk->getSize();
  });

  uint64_t previousEnd = envelope->sectionRVA;
  for (const PlannedChunk &planned : plannedChunks) {
    uint64_t chunkEnd = planned.startRVA;
    if (planned.chunk->getSize() != 0) {
      if (planned.startRVA < previousEnd || planned.startRVA < envelope->sectionRVA ||
          planned.startRVA > UINT64_MAX - uint64_t(planned.chunk->getSize())) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                               "planned chunks exceed preserved section envelope");
        return false;
      }
      chunkEnd = planned.startRVA + uint64_t(planned.chunk->getSize());
      if (chunkEnd > envelope->maxSectionEndRVA) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                               "planned chunks exceed preserved section envelope");
        return false;
      }
      previousEnd = chunkEnd;
    } else if (planned.startRVA < envelope->sectionRVA ||
               planned.startRVA > envelope->maxSectionEndRVA) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "zero-sized placement lies outside preserved section envelope");
      return false;
    }
  }

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
                              IncrementalBaselineData &baseline,
                              OutputSection &currentSection,
                              bool &exactLayoutOnly) {
  uint32_t envelopeIndex = UINT32_MAX;
  const IncrementalSectionEnvelopeState *envelope = findSectionEnvelope(
      baseline.state, currentSection.name, currentSection.header.Characteristics,
      envelopeIndex);
  if (!envelope || !isIncrementalPackedLayout(envelope->layoutKind)) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                           "missing packed-section envelope in incremental state");
    return false;
  }

  StringMap<const IncrementalPlacementState *> placementsByKey;
  for (const IncrementalPlacementState &placement : baseline.state.placements) {
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

    std::string key =
        getIncrementalChunkKey(baseline.currentInputs.inputIndices, *chunk);
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
                                  const IncrementalStateFile &loadedState,
                                  IncrementalLayoutResult &result) {
  result.sizeOfHeaders = loadedState.sizeOfHeaders;
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
  std::unique_ptr<LayoutStableLink> validated = takeActiveLayoutStableLink(ctx);
  if (!validated)
    return false;

  IncrementalBaselineData &baseline = validated->baseline;
  ctx.pendingIncrementalFallback.reset();

  IncrementalReuseData reuse;
  clearReuseState(reuse);
  auto installLayoutFallback = [&]() {
    std::unique_ptr<FullImageBuild> fullImageBuild =
        consumePendingIncrementalFallback(ctx,
                                          std::move(validated->baselineEmission));
    installIncrementalCoordinator(
        ctx, IncrementalCoordinator::makeFullImageBuild(
                 fullImageBuild->fallbackReason,
                 std::move(fullImageBuild->fallbackDetail),
                 std::move(fullImageBuild->baselineEmission)));
    return false;
  };

  SmallVector<OutputSection *, 16> activeSections =
      getActiveSections(ctx, &baseline.state);
  if (activeSections.size() != baseline.state.sections.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "output section count changed");
    return installLayoutFallback();
  }

  for (OutputSection *section : activeSections) {
    IncrementalSectionLayoutKind layoutKind = classifyIncrementalSection(
        section->name, section->header.Characteristics);
    if (!isIncrementalFreeSlotLayout(layoutKind))
      continue;

    uint32_t envelopeIndex = UINT32_MAX;
    const IncrementalSectionEnvelopeState *envelope = findSectionEnvelope(
        baseline.state, section->name, section->header.Characteristics,
        envelopeIndex);
    if (!envelope || !isIncrementalFreeSlotLayout(envelope->layoutKind)) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "missing slot envelope in incremental state");
      return installLayoutFallback();
    }
    if (!validateSlotReuseState(ctx, *envelope, envelopeIndex,
                                baseline.state.slotRecords))
      return installLayoutFallback();
  }

  bool exactLayoutOnly = true;
  SmallVector<std::string, 8> verboseLogs;
  for (size_t sectionIndex = 0; sectionIndex < activeSections.size();
       ++sectionIndex) {
    OutputSection *currentSection = activeSections[sectionIndex];
    const IncrementalSectionState &oldSection =
        baseline.state.sections[sectionIndex];

    if (currentSection->name != oldSection.name ||
        currentSection->header.Characteristics != oldSection.characteristics) {
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "section layout changed: " + currentSection->name);
      return installLayoutFallback();
    }

    IncrementalSectionLayoutKind layoutKind = classifyIncrementalSection(
        currentSection->name, currentSection->header.Characteristics);
    if (isIncrementalFreeSlotLayout(layoutKind) &&
        !hasDuplicateIncrementalChunkKeys(
            baseline.currentInputs.inputIndices, *currentSection) &&
        !(layoutKind == IncrementalSectionLayoutKind::ReadOnlyDataFreeSlots &&
          llvm::any_of(currentSection->chunks,
                       [](const Chunk *chunk) { return isa<MergeChunk>(chunk); }))) {
      if (!planSlotReuseSection(ctx, baseline, reuse, *currentSection,
                                exactLayoutOnly, verboseLogs))
        return installLayoutFallback();
      continue;
    }
    if (isIncrementalPackedLayout(layoutKind)) {
      if (!planPackedSection(ctx, baseline, *currentSection, exactLayoutOnly))
        return installLayoutFallback();
      continue;
    }

    if (!applyExactSectionLayout(ctx, baseline, *currentSection, oldSection,
                                 sectionIndex))
      return installLayoutFallback();
  }

  reuse.currentEdges =
      buildIncrementalEdgeStates(ctx, baseline.currentInputs.inputIndices);

  StringMap<const IncrementalPlacementState *> oldPlacementsByKey;
  for (const IncrementalPlacementState &placement : baseline.state.placements)
    oldPlacementsByKey[placement.key] = &placement;

  StringMap<const IncrementalChunkState *> oldChunksByKey;
  DenseMap<const IncrementalChunkState *, const IncrementalSectionState *>
      oldSectionsByChunk;
  for (const IncrementalSectionState &oldSection : baseline.state.sections) {
    if (oldSection.firstChunk > baseline.state.chunks.size() ||
        baseline.state.chunks.size() - oldSection.firstChunk <
            oldSection.chunkCount)
      continue;
    for (size_t i = 0; i < oldSection.chunkCount; ++i) {
      const IncrementalChunkState &oldChunk =
          baseline.state.chunks[oldSection.firstChunk + i];
      oldChunksByKey[oldChunk.key] = &oldChunk;
      oldSectionsByChunk[&oldChunk] = &oldSection;
    }
  }

  StringSet<> affectedSourceKeys;
  for (const IncrementalEdgeState &edge : reuse.currentEdges) {
    if (!reuse.movedChunkTargets.contains(edge.targetKey) &&
        !reuse.activeRedirectTargets.contains(edge.targetKey))
      continue;
    if (reuse.activeRedirectTargets.contains(edge.targetKey) &&
        edge.routing ==
            IncrementalEdgeRouting::RedirectEligibleEntryReference)
      continue;
    affectedSourceKeys.insert(edge.sourceKey);
  }

  StringRef oldImage = baseline.oldImage->getBuffer();
  ArrayRef<uint8_t> oldBytes(
      reinterpret_cast<const uint8_t *>(oldImage.data()), oldImage.size());
  for (OutputSection *section : activeSections) {
    for (Chunk *chunk : section->chunks) {
      auto *sectionChunk = dyn_cast<SectionChunk>(chunk);
      if (!sectionChunk || !sectionChunk->file)
        continue;

      std::string key =
          getIncrementalChunkKey(baseline.currentInputs.inputIndices,
                                 *sectionChunk);
      auto placementIt = oldPlacementsByKey.find(key);
      auto oldChunkIt = oldChunksByKey.find(key);
      if (placementIt == oldPlacementsByKey.end() || oldChunkIt == oldChunksByKey.end()) {
        reuse.rewrittenChunks.insert(sectionChunk);
        continue;
      }

      auto fileIt = baseline.currentInputs.inputIndices.find(sectionChunk->file);
      if (fileIt == baseline.currentInputs.inputIndices.end() ||
          baseline.changedInputs.contains(sectionChunk->file) ||
          placementIt->second->startRVA != sectionChunk->getRVA() ||
          affectedSourceKeys.contains(key)) {
        reuse.rewrittenChunks.insert(sectionChunk);
        continue;
      }

      const IncrementalChunkState *oldChunk = oldChunkIt->second;
      const IncrementalSectionState *oldSection = oldSectionsByChunk.lookup(oldChunk);
      if (!oldSection) {
        reuse.rewrittenChunks.insert(sectionChunk);
        continue;
      }

      uint64_t fileOffset = oldSection->fileOffset + (oldChunk->rva - oldSection->rva);
      if (fileOffset > oldBytes.size() ||
          oldBytes.size() - fileOffset < sectionChunk->getSize()) {
        setIncrementalFallback(ctx, IncrementalFallbackReason::OutputMismatch,
                               "reused chunk bytes extend past prior image");
        return installLayoutFallback();
      }
      reuse.reusedChunkData[sectionChunk] =
          oldBytes.slice(fileOffset, sectionChunk->getSize());
    }
  }

  rewriteRelocsToRedirectTargets(ctx, baseline.currentInputs.inputIndices, reuse);
  for (OutputSection *section : activeSections) {
    if (classifyIncrementalSection(section->name, section->header.Characteristics) !=
        IncrementalSectionLayoutKind::TextFreeSlots)
      continue;
    if (!validateAmd64Rel32Layout(ctx, reuse.rewrittenChunks, *section))
      return installLayoutFallback();
  }

  recomputeOutputLayout(ctx, activeSections, baseline.state, result);

  if (ctx.config.verbose) {
    Log(ctx) << "incremental: byte-reuse layout active";
    if (exactLayoutOnly)
      Log(ctx) << "incremental: exact-layout reuse active";
    for (const std::string &log : verboseLogs)
      Log(ctx) << log;
  }

  installIncrementalCoordinator(
      ctx, IncrementalCoordinator::makeByteReuseLink(
               std::move(validated->baseline), std::move(reuse),
               std::move(validated->pdbReuse),
               std::move(validated->baselineEmission)));
  return true;
}

} // namespace lld::coff
