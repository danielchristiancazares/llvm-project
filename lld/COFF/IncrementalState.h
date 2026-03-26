#ifndef LLD_COFF_INCREMENTALSTATE_H
#define LLD_COFF_INCREMENTALSTATE_H

#include "Config.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lld::coff {

enum class IncrementalLayoutMode : uint16_t {
  Exact = 1,
  Slotted = 2,
};

enum class IncrementalChunkKind : uint16_t {
  ObjSection = 1,
  Synthetic = 2,
  Padding = 3,
  EntryRedirect = 4,
  LongThunk = 5,
};

enum class IncrementalSymbolKind : uint16_t {
  Regular = 1,
  Common = 2,
  ImportData = 3,
  ImportThunk = 4,
  LocalImport = 5,
  Absolute = 6,
  Synthetic = 7,
};

enum class IncrementalSectionLayoutKind : uint16_t {
  ExactSectionLayout = 1,
  TextFreeSlots = 2,
  ReadOnlyDataFreeSlots = 3,
  WritableDataFreeSlots = 4,
  PackedPDataPrefix = 5,
  PackedXDataPrefix = 6,
};

enum class IncrementalSlotState : uint16_t {
  Occupied = 1,
  Free = 2,
};

enum class IncrementalPlacementKind : uint16_t {
  ExistingSlot = 1,
  ReusedFreeSlot = 2,
  TailReserve = 3,
  PackedPrefix = 4,
};

enum class IncrementalEdgeRouting : uint16_t {
  BodyOnlyReference = 1,
  RedirectEligibleEntryReference = 2,
};

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
  IncrementalChunkKind kind = IncrementalChunkKind::Synthetic;
  std::string key;
  uint32_t sectionIndex = 0;
  uint32_t inputIndex = UINT32_MAX;
  uint32_t outputCharacteristics = 0;
  uint32_t sectionNumber = 0;
  uint32_t alignment = 1;
  uint64_t rva = 0;
  uint64_t size = 0;
  uint64_t slotCapacity = 0;
  uint64_t contentHash = 0;
  uint64_t symbolHash = 0;
};

struct IncrementalSymbolState {
  std::string name;
  std::string auxiliaryKey;
  IncrementalSymbolKind kind = IncrementalSymbolKind::Regular;
  uint32_t inputIndex = UINT32_MAX;
  uint64_t value = 0;
};

struct IncrementalSectionEnvelopeState {
  std::string name;
  uint32_t characteristics = 0;
  uint64_t sectionRVA = 0;
  uint64_t maxSectionEndRVA = 0;
  uint64_t activeEndRVA = 0;
  IncrementalSectionLayoutKind layoutKind =
      IncrementalSectionLayoutKind::ExactSectionLayout;
};

struct IncrementalSlotRecordState {
  uint32_t envelopeIndex = UINT32_MAX;
  uint64_t startRVA = 0;
  uint64_t capacity = 0;
  uint64_t committedSize = 0;
  uint32_t minAlignment = 1;
  uint8_t fillByte = 0;
  IncrementalSlotState state = IncrementalSlotState::Free;
  std::string occupantKey;
};

struct IncrementalPackedSectionState {
  uint32_t envelopeIndex = UINT32_MAX;
  uint64_t activePrefixSize = 0;
  uint64_t reserveSize = 0;
  std::vector<std::string> recordKeys;
};

struct IncrementalPlacementState {
  std::string key;
  uint32_t envelopeIndex = UINT32_MAX;
  IncrementalPlacementKind kind = IncrementalPlacementKind::ExistingSlot;
  uint64_t startRVA = 0;
  uint64_t size = 0;
  uint32_t alignment = 1;
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

struct IncrementalStateFile {
  uint32_t version = 6;
  IncrementalLayoutMode layoutMode = IncrementalLayoutMode::Slotted;
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
  std::vector<IncrementalSectionState> sections;
  std::vector<IncrementalChunkState> chunks;
  std::vector<IncrementalSymbolState> symbols;
  std::vector<IncrementalSectionEnvelopeState> sectionEnvelopes;
  std::vector<IncrementalSlotRecordState> slotRecords;
  std::vector<IncrementalPackedSectionState> packedSections;
  std::vector<IncrementalPlacementState> placements;
  std::vector<IncrementalTextRedirectState> textRedirects;
  IncrementalTextThunkPoolState textThunkPool;
};

llvm::Expected<IncrementalStateFile> loadIncrementalState(llvm::StringRef path);
llvm::Error writeIncrementalState(llvm::StringRef path,
                                  const IncrementalStateFile &state);

} // namespace lld::coff

#endif
