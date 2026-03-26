#ifndef LLD_COFF_INCREMENTALSTATE_H
#define LLD_COFF_INCREMENTALSTATE_H

#include "Config.h"
#include "lld/Common/Closed.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lld::coff {

struct IncrementalInputState {
  std::string name;
  std::string parentName;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  uint64_t size = 0;
};

struct IncrementalSectionState {
  std::string name;
  uint32_t characteristics = 0;
  uint64_t rva = 0;
  uint64_t fileOffset = 0;
  uint64_t virtualSize = 0;
  uint64_t rawSize = 0;
  uint32_t firstChunk = 0;
  uint32_t chunkCount = 0;
};

struct IncrementalChunkState {
  std::string key;
  uint32_t sectionIndex = 0;
  uint32_t outputCharacteristics = 0;
  uint32_t alignment = 1;
  uint64_t rva = 0;
  uint64_t size = 0;
  uint64_t slotCapacity = 0;
};

struct ObjSectionChunkSnapshot final {
  IncrementalChunkState chunk;
  uint32_t inputIndex = 0;
  uint32_t sectionNumber = 0;
  uint64_t contentHash = 0;
  uint64_t symbolHash = 0;
};

struct SyntheticChunkSnapshot final {
  IncrementalChunkState chunk;
};

struct PaddingChunkSnapshot final {
  IncrementalChunkState chunk;
};

struct EntryRedirectChunkSnapshot final {
  IncrementalChunkState chunk;
};

struct LongThunkChunkSnapshot final {
  IncrementalChunkState chunk;
};

using IncrementalChunkSnapshot =
    lld::Closed<ObjSectionChunkSnapshot, SyntheticChunkSnapshot,
                PaddingChunkSnapshot, EntryRedirectChunkSnapshot,
                LongThunkChunkSnapshot>;

struct PersistedInputOwner final {
  uint32_t inputIndex = 0;
};

struct NoPersistedInputOwner final {};

using IncrementalPersistedInputOwner =
    lld::Closed<PersistedInputOwner, NoPersistedInputOwner>;

struct PersistedChunkReference final {
  std::string chunkKey;
};

struct NoPersistedChunkReference final {};

using IncrementalPersistedChunkReference =
    lld::Closed<PersistedChunkReference, NoPersistedChunkReference>;

struct RegularResolvedSymbol final {
  std::string name;
  IncrementalPersistedInputOwner owner =
      IncrementalPersistedInputOwner::make<NoPersistedInputOwner>();
  uint64_t value = 0;
  IncrementalPersistedChunkReference chunk =
      IncrementalPersistedChunkReference::make<NoPersistedChunkReference>();
};

struct CommonResolvedSymbol final {
  std::string name;
  IncrementalPersistedInputOwner owner =
      IncrementalPersistedInputOwner::make<NoPersistedInputOwner>();
  uint64_t size = 0;
  uint32_t alignment = 1;
};

struct ImportDataResolvedSymbol final {
  std::string name;
  uint64_t ordinal = 0;
  std::string dllName;
  std::string externalName;
  uint16_t typeInfo = 0;
};

struct ImportThunkResolvedSymbol final {
  std::string name;
  std::string wrappedSymbolName;
};

struct LocalImportResolvedSymbol final {
  std::string name;
  IncrementalPersistedChunkReference chunk =
      IncrementalPersistedChunkReference::make<NoPersistedChunkReference>();
};

struct AbsoluteResolvedSymbol final {
  std::string name;
  uint64_t value = 0;
};

struct SyntheticResolvedSymbol final {
  std::string name;
  IncrementalPersistedChunkReference chunk =
      IncrementalPersistedChunkReference::make<NoPersistedChunkReference>();
};

using IncrementalResolvedSymbolSnapshot =
    lld::Closed<RegularResolvedSymbol, CommonResolvedSymbol,
                ImportDataResolvedSymbol, ImportThunkResolvedSymbol,
                LocalImportResolvedSymbol, AbsoluteResolvedSymbol,
                SyntheticResolvedSymbol>;

struct IncrementalPreservedSlotState {
  uint64_t startRVA = 0;
  uint64_t capacity = 0;
  uint64_t committedSize = 0;
  uint32_t minAlignment = 1;
  uint8_t fillByte = 0;
};

struct FreeSlotRecord final {
  IncrementalPreservedSlotState slot;
};

struct OccupiedSlotRecord final {
  IncrementalPreservedSlotState slot;
  std::string occupantKey;
};

using IncrementalPreservedSlot = lld::Closed<FreeSlotRecord, OccupiedSlotRecord>;

struct ExistingSlotChunkPlacement final {
  std::string key;
  uint64_t startRVA = 0;
  uint64_t size = 0;
  uint32_t alignment = 1;
};

struct PackedPrefixChunkPlacement final {
  std::string key;
  uint64_t startRVA = 0;
  uint64_t size = 0;
  uint32_t alignment = 1;
};

enum class IncrementalEdgeRouting : uint16_t {
  BodyOnlyReference = 1,
  RedirectEligibleEntryReference = 2,
};

struct IncrementalEdgeState {
  std::string sourceKey;
  std::string targetKey;
  IncrementalEdgeRouting routing = IncrementalEdgeRouting::BodyOnlyReference;
  uint32_t sourceOffset = 0;
  uint32_t targetOffset = 0;
};

struct IncrementalTextRedirectState {
  std::string targetKey;
  std::string canonicalSymbol;
  uint64_t redirectRVA = 0;
  uint64_t redirectCapacity = 0;
  uint64_t bodyRVA = 0;
  uint64_t poolThunkRVA = 0;
};

struct IncrementalTextThunkPoolState {
  uint64_t poolStartRVA = 0;
  uint64_t poolEndRVA = 0;
  uint64_t nextFreeRVA = 0;
};

struct IncrementalSlotSectionSnapshot {
  IncrementalSectionState section;
  uint64_t maxSectionEndRVA = 0;
  uint64_t activeEndRVA = 0;
  std::vector<IncrementalPreservedSlot> slots;
  std::vector<ExistingSlotChunkPlacement> preservedChunks;
};

struct TextSlotSectionSnapshot final {
  IncrementalSlotSectionSnapshot slotSection;
  std::vector<IncrementalTextRedirectState> redirects;
  IncrementalTextThunkPoolState thunkPool;
};

struct ReadOnlySlotSectionSnapshot final {
  IncrementalSlotSectionSnapshot slotSection;
};

struct WritableSlotSectionSnapshot final {
  IncrementalSlotSectionSnapshot slotSection;
};

struct IncrementalPackedPrefixSectionSnapshot {
  IncrementalSectionState section;
  uint64_t activePrefixSize = 0;
  uint64_t reserveSize = 0;
  std::vector<PackedPrefixChunkPlacement> members;
};

struct PDataPackedPrefixSectionSnapshot final {
  IncrementalPackedPrefixSectionSnapshot packedSection;
};

struct XDataPackedPrefixSectionSnapshot final {
  IncrementalPackedPrefixSectionSnapshot packedSection;
};

struct ExactSectionSnapshot final {
  IncrementalSectionState section;
};

using IncrementalSectionSnapshot =
    lld::Closed<ExactSectionSnapshot, TextSlotSectionSnapshot,
                ReadOnlySlotSectionSnapshot, WritableSlotSectionSnapshot,
                PDataPackedPrefixSectionSnapshot,
                XDataPackedPrefixSectionSnapshot>;

struct IncrementalBaselineSnapshot {
  llvm::COFF::MachineTypes machine = IMAGE_FILE_MACHINE_UNKNOWN;
  uint64_t outputHash = 0;
  uint64_t outputSize = 0;
  uint64_t hardConfigHash = 0;
  uint64_t softConfigHash = 0;
  uint64_t importTopologyHash = 0;
  uint64_t exportTopologyHash = 0;
  uint64_t resourceInputHash = 0;
  uint64_t sizeOfHeaders = 0;
  uint64_t sizeOfImage = 0;
  std::string outputPath;
  std::vector<IncrementalInputState> inputs;
  std::vector<IncrementalSectionSnapshot> sections;
  std::vector<IncrementalChunkSnapshot> chunks;
  std::vector<IncrementalResolvedSymbolSnapshot> symbols;
};

inline const IncrementalChunkState &
getIncrementalChunkState(const IncrementalChunkSnapshot &chunk) {
  return chunk.match(
      [](const ObjSectionChunkSnapshot &obj) -> const IncrementalChunkState & {
        return obj.chunk;
      },
      [](const SyntheticChunkSnapshot &synthetic)
          -> const IncrementalChunkState & { return synthetic.chunk; },
      [](const PaddingChunkSnapshot &padding) -> const IncrementalChunkState & {
        return padding.chunk;
      },
      [](const EntryRedirectChunkSnapshot &redirect)
          -> const IncrementalChunkState & { return redirect.chunk; },
      [](const LongThunkChunkSnapshot &thunk) -> const IncrementalChunkState & {
        return thunk.chunk;
      });
}

inline const IncrementalSectionState &
getIncrementalSectionState(const IncrementalSectionSnapshot &section) {
  return section.match(
      [](const ExactSectionSnapshot &exact) -> const IncrementalSectionState & {
        return exact.section;
      },
      [](const TextSlotSectionSnapshot &text) -> const IncrementalSectionState & {
        return text.slotSection.section;
      },
      [](const ReadOnlySlotSectionSnapshot &rdata)
          -> const IncrementalSectionState & {
        return rdata.slotSection.section;
      },
      [](const WritableSlotSectionSnapshot &data)
          -> const IncrementalSectionState & {
        return data.slotSection.section;
      },
      [](const PDataPackedPrefixSectionSnapshot &pdata)
          -> const IncrementalSectionState & {
        return pdata.packedSection.section;
      },
      [](const XDataPackedPrefixSectionSnapshot &xdata)
          -> const IncrementalSectionState & {
        return xdata.packedSection.section;
      });
}

inline const IncrementalSlotSectionSnapshot *
getIncrementalSlotSectionSnapshot(const IncrementalSectionSnapshot &section) {
  return section.match(
      [](const ExactSectionSnapshot &) -> const IncrementalSlotSectionSnapshot * {
        return nullptr;
      },
      [](const TextSlotSectionSnapshot &text)
          -> const IncrementalSlotSectionSnapshot * {
        return &text.slotSection;
      },
      [](const ReadOnlySlotSectionSnapshot &rdata)
          -> const IncrementalSlotSectionSnapshot * {
        return &rdata.slotSection;
      },
      [](const WritableSlotSectionSnapshot &data)
          -> const IncrementalSlotSectionSnapshot * {
        return &data.slotSection;
      },
      [](const PDataPackedPrefixSectionSnapshot &)
          -> const IncrementalSlotSectionSnapshot * { return nullptr; },
      [](const XDataPackedPrefixSectionSnapshot &)
          -> const IncrementalSlotSectionSnapshot * { return nullptr; });
}

inline const TextSlotSectionSnapshot *
getTextSlotSectionSnapshot(const IncrementalSectionSnapshot &section) {
  return section.match(
      [](const ExactSectionSnapshot &) -> const TextSlotSectionSnapshot * {
        return nullptr;
      },
      [](const TextSlotSectionSnapshot &text)
          -> const TextSlotSectionSnapshot * { return &text; },
      [](const ReadOnlySlotSectionSnapshot &)
          -> const TextSlotSectionSnapshot * { return nullptr; },
      [](const WritableSlotSectionSnapshot &)
          -> const TextSlotSectionSnapshot * { return nullptr; },
      [](const PDataPackedPrefixSectionSnapshot &)
          -> const TextSlotSectionSnapshot * { return nullptr; },
      [](const XDataPackedPrefixSectionSnapshot &)
          -> const TextSlotSectionSnapshot * { return nullptr; });
}

inline const IncrementalPackedPrefixSectionSnapshot *
getIncrementalPackedPrefixSectionSnapshot(const IncrementalSectionSnapshot &section) {
  return section.match(
      [](const ExactSectionSnapshot &)
          -> const IncrementalPackedPrefixSectionSnapshot * { return nullptr; },
      [](const TextSlotSectionSnapshot &)
          -> const IncrementalPackedPrefixSectionSnapshot * { return nullptr; },
      [](const ReadOnlySlotSectionSnapshot &)
          -> const IncrementalPackedPrefixSectionSnapshot * { return nullptr; },
      [](const WritableSlotSectionSnapshot &)
          -> const IncrementalPackedPrefixSectionSnapshot * { return nullptr; },
      [](const PDataPackedPrefixSectionSnapshot &pdata)
          -> const IncrementalPackedPrefixSectionSnapshot * {
        return &pdata.packedSection;
      },
      [](const XDataPackedPrefixSectionSnapshot &xdata)
          -> const IncrementalPackedPrefixSectionSnapshot * {
        return &xdata.packedSection;
      });
}

inline const IncrementalPreservedSlotState &
getIncrementalPreservedSlotState(const IncrementalPreservedSlot &slot) {
  return slot.match(
      [](const FreeSlotRecord &freeSlot) -> const IncrementalPreservedSlotState & {
        return freeSlot.slot;
      },
      [](const OccupiedSlotRecord &occupied)
          -> const IncrementalPreservedSlotState & { return occupied.slot; });
}

inline const std::string *
getIncrementalPreservedSlotOccupant(const IncrementalPreservedSlot &slot) {
  return slot.match(
      [](const FreeSlotRecord &) -> const std::string * { return nullptr; },
      [](const OccupiedSlotRecord &occupied) -> const std::string * {
        return &occupied.occupantKey;
      });
}

llvm::Expected<IncrementalBaselineSnapshot>
loadIncrementalState(llvm::StringRef path);
llvm::Error writeIncrementalState(llvm::StringRef path,
                                  const IncrementalBaselineSnapshot &state);

} // namespace lld::coff

#endif
