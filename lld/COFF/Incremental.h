//===- Incremental.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INCREMENTAL_H
#define LLD_COFF_INCREMENTAL_H

#include "IncrementalState.h"
#include "lld/Common/Closed.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MathExtras.h"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class MemoryBuffer;
}

namespace lld::coff {

class Chunk;
class COFFLinkerContext;
class Defined;
class ObjFile;
class SectionChunk;

using IncrementalInputIndexMap = llvm::DenseMap<const ObjFile *, uint32_t>;

enum class IncrementalSectionLayoutKind : uint8_t {
  ExactSectionLayout = 1,
  TextFreeSlots = 2,
  ReadOnlyDataFreeSlots = 3,
  WritableDataFreeSlots = 4,
  PackedPDataPrefix = 5,
  PackedXDataPrefix = 6,
};

enum class IncrementalChunkKind : uint8_t {
  ObjSection = 1,
  Synthetic = 2,
  Padding = 3,
  EntryRedirect = 4,
  LongThunk = 5,
};

struct EmitNextBaseline final {};
struct SkipNextBaseline final {};
using IncrementalBaselineEmission =
    lld::Closed<EmitNextBaseline, SkipNextBaseline>;

struct ReusePdbMetadata final {};
struct RebuildPdbMetadata final {};
using IncrementalPdbReusePolicy =
    lld::Closed<ReusePdbMetadata, RebuildPdbMetadata>;

struct IncrementalCurrentInputs {
  std::vector<uint64_t> hashes;
  std::vector<std::string> names;
  std::vector<std::string> parentNames;
  std::vector<uint64_t> archiveOffsets;
  IncrementalInputIndexMap inputIndices;
};

struct IncrementalOutputMetadata {
  uint32_t timestamp = 0;
  std::optional<llvm::codeview::GUID> pdbGuid;
  uint32_t pdbAge = 1;
};

struct IncrementalBaselineData {
  IncrementalBaselineSnapshot snapshot;
  std::unique_ptr<llvm::MemoryBuffer> oldImage;
  IncrementalOutputMetadata previousOutputMetadata;
  IncrementalCurrentInputs currentInputs;
  llvm::DenseSet<const ObjFile *> changedInputs;
  llvm::StringSet<> expectedArchiveMembers;
  llvm::StringSet<> loadedArchiveMembers;
};

struct IncrementalReuseData {
  llvm::DenseMap<const SectionChunk *, llvm::ArrayRef<uint8_t>> reusedChunkData;
  llvm::DenseSet<const Chunk *> rewrittenChunks;
  std::vector<IncrementalEdgeState> currentEdges;
  std::vector<IncrementalTextRedirectState> currentTextRedirects;
  IncrementalTextThunkPoolState currentTextThunkPool;
  llvm::StringMap<Defined *> redirectSymbols;
  llvm::StringMap<Defined *> poolThunkSymbols;
  llvm::StringSet<> movedChunkTargets;
  llvm::StringSet<> activeRedirectTargets;
  bool exactLayoutOnly = false;
};

enum class IncrementalRedirectEngagement : uint8_t {
  Deferred = 1,
  Installed = 2,
};

enum class IncrementalRedirectProvenance : uint8_t {
  FreshInstall = 1,
  LegacyRedirect = 2,
};

enum class IncrementalRedirectTargeting : uint8_t {
  DirectBodyTarget = 1,
  PoolThunkTarget = 2,
};

struct IncrementalTextThunkPlanState {
  uint64_t redirectRVA = 0;
  uint64_t bodyRVA = 0;
  uint64_t poolThunkRVA = 0;
  IncrementalRedirectEngagement engagement =
      IncrementalRedirectEngagement::Deferred;
  IncrementalRedirectProvenance provenance =
      IncrementalRedirectProvenance::FreshInstall;
  IncrementalRedirectTargeting targeting =
      IncrementalRedirectTargeting::DirectBodyTarget;
};

struct IncrementalDisabled final {};

struct RebuildForMissingBaseline final {};
struct RebuildForRejectedBaseline final {};
struct RebuildForUnsupportedMachine final {};
struct RebuildForBitcodeInputs final {};
struct RebuildForTailMerging final {};
struct RebuildForConfigDrift final {};
struct RebuildForOutputDrift final {};
struct RebuildForLayoutRewrite final {};
struct RebuildForSlotCapacity final {};
struct RebuildForMergeParticipantDrift final {};
struct RebuildForPackedSectionGrowth final {};
struct RebuildForRel32RangeOverflow final {};
using IncrementalFullBuildCause =
    lld::Closed<RebuildForMissingBaseline, RebuildForRejectedBaseline,
                RebuildForUnsupportedMachine, RebuildForBitcodeInputs,
                RebuildForTailMerging, RebuildForConfigDrift,
                RebuildForOutputDrift, RebuildForLayoutRewrite,
                RebuildForSlotCapacity, RebuildForMergeParticipantDrift,
                RebuildForPackedSectionGrowth, RebuildForRel32RangeOverflow>;

struct IncrementalFullBuildDecision final {
  IncrementalFullBuildCause cause;
  std::string message;
};

template <class Cause>
IncrementalFullBuildDecision
makeIncrementalFullBuildDecision(Cause cause, const llvm::Twine &message) {
  return IncrementalFullBuildDecision{
      IncrementalFullBuildCause::make<Cause>(std::move(cause)), message.str()};
}

inline IncrementalFullBuildDecision
rebuildForMissingBaseline(const llvm::Twine &message = "MissingState") {
  return makeIncrementalFullBuildDecision(RebuildForMissingBaseline{}, message);
}

inline IncrementalFullBuildDecision
rebuildForRejectedBaseline(const llvm::Twine &message = "InvalidState") {
  return makeIncrementalFullBuildDecision(RebuildForRejectedBaseline{},
                                          message);
}

inline IncrementalFullBuildDecision rebuildForUnsupportedMachine(
    const llvm::Twine &message = "UnsupportedMachine") {
  return makeIncrementalFullBuildDecision(RebuildForUnsupportedMachine{},
                                          message);
}

inline IncrementalFullBuildDecision
rebuildForBitcodeInputs(const llvm::Twine &message = "LtoInput") {
  return makeIncrementalFullBuildDecision(RebuildForBitcodeInputs{}, message);
}

inline IncrementalFullBuildDecision
rebuildForTailMerging(const llvm::Twine &message = "TailMergeEnabled") {
  return makeIncrementalFullBuildDecision(RebuildForTailMerging{}, message);
}

inline IncrementalFullBuildDecision
rebuildForConfigDrift(const llvm::Twine &message = "ConfigChanged") {
  return makeIncrementalFullBuildDecision(RebuildForConfigDrift{}, message);
}

inline IncrementalFullBuildDecision
rebuildForOutputDrift(const llvm::Twine &message = "OutputMismatch") {
  return makeIncrementalFullBuildDecision(RebuildForOutputDrift{}, message);
}

inline IncrementalFullBuildDecision
rebuildForLayoutRewrite(const llvm::Twine &message = "LayoutChanged") {
  return makeIncrementalFullBuildDecision(RebuildForLayoutRewrite{}, message);
}

inline IncrementalFullBuildDecision
rebuildForSlotCapacity(const llvm::Twine &message = "SlotOverflow") {
  return makeIncrementalFullBuildDecision(RebuildForSlotCapacity{}, message);
}

inline IncrementalFullBuildDecision rebuildForMergeParticipantDrift(
    const llvm::Twine &message = "MergeChunkParticipantChanged") {
  return makeIncrementalFullBuildDecision(RebuildForMergeParticipantDrift{},
                                          message);
}

inline IncrementalFullBuildDecision rebuildForPackedSectionGrowth(
    const llvm::Twine &message = "PackedSectionOverflow") {
  return makeIncrementalFullBuildDecision(RebuildForPackedSectionGrowth{},
                                          message);
}

inline IncrementalFullBuildDecision rebuildForRel32RangeOverflow(
    const llvm::Twine &message = "Amd64Rel32OutOfRange") {
  return makeIncrementalFullBuildDecision(RebuildForRel32RangeOverflow{},
                                          message);
}

struct PendingFullImageBuild final {
  IncrementalBaselineEmission baselineEmission;
};

struct FullImageBuild final {
  IncrementalFullBuildDecision decision;
  IncrementalBaselineEmission baselineEmission;
};

struct StateBackedLink final {
  IncrementalBaselineData baseline;
  IncrementalPdbReusePolicy pdbReuse =
      IncrementalPdbReusePolicy::make<RebuildPdbMetadata>();
  IncrementalBaselineEmission baselineEmission =
      IncrementalBaselineEmission::make<EmitNextBaseline>();
};

struct LayoutStableLink final {
  IncrementalBaselineData baseline;
  IncrementalPdbReusePolicy pdbReuse =
      IncrementalPdbReusePolicy::make<RebuildPdbMetadata>();
  IncrementalBaselineEmission baselineEmission =
      IncrementalBaselineEmission::make<EmitNextBaseline>();
};

struct ByteReuseLink final {
  IncrementalBaselineData baseline;
  IncrementalReuseData reuse;
  IncrementalPdbReusePolicy pdbReuse =
      IncrementalPdbReusePolicy::make<RebuildPdbMetadata>();
  IncrementalBaselineEmission baselineEmission =
      IncrementalBaselineEmission::make<EmitNextBaseline>();
};

class IncrementalCoordinator final {
public:
  IncrementalCoordinator(const IncrementalCoordinator &) = delete;
  IncrementalCoordinator &operator=(const IncrementalCoordinator &) = delete;
  IncrementalCoordinator(IncrementalCoordinator &&) = default;
  IncrementalCoordinator &operator=(IncrementalCoordinator &&) = delete;

  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator> makeDisabled();
  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator>
  makePendingFullImageBuild(IncrementalBaselineEmission baselineEmission);
  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator>
  makeFullImageBuild(IncrementalFullBuildDecision decision,
                     IncrementalBaselineEmission baselineEmission);
  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator>
  makeStateBackedLink(IncrementalBaselineData baseline,
                      IncrementalPdbReusePolicy pdbReuse,
                      IncrementalBaselineEmission baselineEmission);
  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator>
  makeLayoutStableLink(IncrementalBaselineData baseline,
                       IncrementalPdbReusePolicy pdbReuse,
                       IncrementalBaselineEmission baselineEmission);
  [[nodiscard]] static std::unique_ptr<IncrementalCoordinator>
  makeByteReuseLink(IncrementalBaselineData baseline,
                    IncrementalReuseData reuse,
                    IncrementalPdbReusePolicy pdbReuse,
                    IncrementalBaselineEmission baselineEmission);

  template <class... Fs> decltype(auto) match(Fs &&...Fns) & {
    return state.match(std::forward<Fs>(Fns)...);
  }

  template <class... Fs> decltype(auto) match(Fs &&...Fns) const & {
    return state.match(std::forward<Fs>(Fns)...);
  }

private:
  using State =
      lld::Closed<IncrementalDisabled, PendingFullImageBuild, FullImageBuild,
                  StateBackedLink, LayoutStableLink, ByteReuseLink>;

  explicit IncrementalCoordinator(State &&state) : state(std::move(state)) {}

  State state;
};

void prepareIncrementalLink(COFFLinkerContext &ctx);
void finalizeIncrementalLinkPlan(COFFLinkerContext &ctx);
void finalizeIncrementalLink(COFFLinkerContext &ctx);
void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      llvm::StringRef archiveName,
                                      uint64_t archiveOffset,
                                      llvm::StringRef memberName);

void installIncrementalCoordinator(
    COFFLinkerContext &ctx,
    std::unique_ptr<IncrementalCoordinator> coordinator);
void installIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision,
    IncrementalBaselineEmission baselineEmission);
void installPendingIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision);
void installPendingIncrementalFullImageBuild(
    COFFLinkerContext &ctx, IncrementalFullBuildDecision decision,
    IncrementalBaselineEmission baselineEmission);
uint64_t computeIncrementalHardConfigHash(const Configuration &config);
uint64_t computeIncrementalSoftConfigHash(
    const Configuration &config,
    std::optional<uint32_t> timestampOverride = std::nullopt);

IncrementalCurrentInputs
prepareCurrentIncrementalInputs(COFFLinkerContext &ctx);
const IncrementalCurrentInputs *
findActiveIncrementalCurrentInputs(const COFFLinkerContext &ctx);
const IncrementalBaselineData *
findActiveIncrementalBaseline(const COFFLinkerContext &ctx);
const IncrementalOutputMetadata *
findActiveIncrementalOutputMetadata(const COFFLinkerContext &ctx);
bool shouldPreserveIncrementalBuildMetadata(const COFFLinkerContext &ctx);
bool shouldReuseIncrementalPdbMetadata(const COFFLinkerContext &ctx);
bool shouldSkipIncrementalPdbEmission(const COFFLinkerContext &ctx);
IncrementalSectionLayoutKind
classifyIncrementalSection(llvm::StringRef name, uint32_t characteristics);
bool isIncrementalFreeSlotLayout(IncrementalSectionLayoutKind layoutKind);
bool isIncrementalPackedLayout(IncrementalSectionLayoutKind layoutKind);
uint8_t getIncrementalFillByte(IncrementalSectionLayoutKind layoutKind);
bool isIncrementalPersistedSlotChunk(IncrementalSectionLayoutKind layoutKind,
                                     const Chunk &chunk);
template <class OnSelectedSlot, class OnNoReusableSlot>
decltype(auto)
matchBestFitIncrementalFreeSlot(llvm::ArrayRef<IncrementalPreservedSlot> slots,
                                uint64_t size, uint32_t alignment,
                                OnSelectedSlot &&onSelectedSlot,
                                OnNoReusableSlot &&onNoReusableSlot) {
  size_t bestIndex = 0;
  bool found = false;
  for (size_t i = 0; i < slots.size(); ++i) {
    const IncrementalPreservedSlotState &slot =
        getIncrementalPreservedSlotState(slots[i]);
    if (slot.capacity < size || slot.startRVA % alignment != 0)
      continue;
    if (!found ||
        slot.capacity <
            getIncrementalPreservedSlotState(slots[bestIndex]).capacity ||
        (slot.capacity ==
             getIncrementalPreservedSlotState(slots[bestIndex]).capacity &&
         slot.startRVA <
             getIncrementalPreservedSlotState(slots[bestIndex]).startRVA)) {
      bestIndex = i;
      found = true;
    }
  }
  if (!found)
    return std::forward<OnNoReusableSlot>(onNoReusableSlot)();
  return std::forward<OnSelectedSlot>(onSelectedSlot)(bestIndex);
}

template <class OnAllocatedTailReserve, class OnExhaustedTailReserve>
decltype(auto)
matchIncrementalTailReserve(uint64_t tailCursor, uint64_t maxSectionEndRVA,
                            uint64_t size, uint32_t alignment,
                            OnAllocatedTailReserve &&onAllocatedTailReserve,
                            OnExhaustedTailReserve &&onExhaustedTailReserve) {
  uint64_t startRVA = llvm::alignTo(tailCursor, uint64_t(alignment));
  if (startRVA > maxSectionEndRVA || maxSectionEndRVA - startRVA < size)
    return std::forward<OnExhaustedTailReserve>(onExhaustedTailReserve)();
  return std::forward<OnAllocatedTailReserve>(onAllocatedTailReserve)(startRVA);
}

template <class OnSelectedPoolThunk, class OnNoPoolThunk>
decltype(auto) matchIncrementalTextThunkRVA(
    uint64_t oldPoolThunkRVA, uint64_t tailCursor, uint64_t poolCursor,
    uint64_t poolEndRVA, llvm::ArrayRef<uint64_t> claimedThunkRVAs,
    OnSelectedPoolThunk &&onSelectedPoolThunk, OnNoPoolThunk &&onNoPoolThunk,
    llvm::ArrayRef<uint64_t> freedThunkRVAs = {}) {
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
    return std::forward<OnSelectedPoolThunk>(onSelectedPoolThunk)(
        oldPoolThunkRVA);

  uint64_t bestFreedThunkRVA = 0;
  bool foundFreedThunk = false;
  for (uint64_t freedThunkRVA : freedThunkRVAs) {
    if (!canReuse(freedThunkRVA))
      continue;
    if (!foundFreedThunk || freedThunkRVA > bestFreedThunkRVA) {
      bestFreedThunkRVA = freedThunkRVA;
      foundFreedThunk = true;
    }
  }
  if (foundFreedThunk)
    return std::forward<OnSelectedPoolThunk>(onSelectedPoolThunk)(
        bestFreedThunkRVA);

  uint64_t nextCursor = poolCursor;
  while (nextCursor > tailCursor && nextCursor - tailCursor >= thunkSize) {
    uint64_t candidate = (nextCursor - thunkSize) & ~(thunkSize - 1);
    if (candidate < tailCursor)
      break;
    if (candidate <= poolEndRVA && poolEndRVA - candidate >= thunkSize &&
        !isClaimed(candidate))
      return std::forward<OnSelectedPoolThunk>(onSelectedPoolThunk)(candidate);
    nextCursor = candidate;
  }
  return std::forward<OnNoPoolThunk>(onNoPoolThunk)();
}

void planIncrementalTextThunkAssignments(
    llvm::MutableArrayRef<IncrementalTextThunkPlanState> plans,
    uint64_t tailCursor, uint64_t &poolCursor, uint64_t &poolStart,
    uint64_t poolEndRVA, bool allowPoolThunks);
bool isIncrementalAmd64Rel32InRange(uint16_t type, uint64_t sourceRVA,
                                    uint64_t targetRVA);
IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk);
std::string getIncrementalChunkKey(const IncrementalInputIndexMap &inputIndices,
                                   const Chunk &chunk);
uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk);

} // namespace lld::coff

#endif
