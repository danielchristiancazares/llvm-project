#include "IncrementalState.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>
#include <limits>

using namespace llvm;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {

namespace {

constexpr char stateMagic[8] = {'L', 'L', 'I', 'L', 'K', '6', '4', '\0'};
constexpr uint32_t exactStateVersion = 2;
constexpr uint32_t slottedStateVersion = 3;
constexpr uint32_t redirectStateVersion = 4;
constexpr uint16_t envelopeFlagPackedActivePrefix = 1u << 0;
constexpr uint16_t envelopeFlagSlotReuseEnabled = 1u << 1;

struct FileHeaderV2 {
  char magic[8];
  ulittle32_t version;
  ulittle16_t machine;
  ulittle16_t flags;
  ulittle64_t outputHash;
  ulittle64_t outputSize;
  ulittle64_t hardConfigHash;
  ulittle64_t softConfigHash;
  ulittle64_t importTopologyHash;
  ulittle64_t exportTopologyHash;
  ulittle64_t resourceInputHash;
  ulittle64_t sizeOfHeaders;
  ulittle64_t sizeOfImage;
  ulittle64_t outputPathOffset;
  ulittle64_t inputTableOffset;
  ulittle64_t inputCount;
  ulittle64_t sectionTableOffset;
  ulittle64_t sectionCount;
  ulittle64_t chunkTableOffset;
  ulittle64_t chunkCount;
  ulittle64_t symbolTableOffset;
  ulittle64_t symbolCount;
  ulittle64_t stringTableOffset;
  ulittle64_t stringTableSize;
};

struct FileHeaderV3 {
  char magic[8];
  ulittle32_t version;
  ulittle16_t machine;
  ulittle16_t flags;
  ulittle16_t layoutMode;
  ulittle16_t reserved0;
  ulittle64_t outputHash;
  ulittle64_t outputSize;
  ulittle64_t hardConfigHash;
  ulittle64_t softConfigHash;
  ulittle64_t importTopologyHash;
  ulittle64_t exportTopologyHash;
  ulittle64_t resourceInputHash;
  ulittle64_t sizeOfHeaders;
  ulittle64_t sizeOfImage;
  ulittle64_t outputPathOffset;
  ulittle64_t inputTableOffset;
  ulittle64_t inputCount;
  ulittle64_t sectionTableOffset;
  ulittle64_t sectionCount;
  ulittle64_t chunkTableOffset;
  ulittle64_t chunkCount;
  ulittle64_t symbolTableOffset;
  ulittle64_t symbolCount;
  ulittle64_t envelopeTableOffset;
  ulittle64_t envelopeCount;
  ulittle64_t slotTableOffset;
  ulittle64_t slotCount;
  ulittle64_t packedSectionTableOffset;
  ulittle64_t packedSectionCount;
  ulittle64_t packedKeyTableOffset;
  ulittle64_t packedKeyCount;
  ulittle64_t placementTableOffset;
  ulittle64_t placementCount;
  ulittle64_t stringTableOffset;
  ulittle64_t stringTableSize;
};

struct FileHeaderV4 {
  char magic[8];
  ulittle32_t version;
  ulittle16_t machine;
  ulittle16_t flags;
  ulittle16_t layoutMode;
  ulittle16_t reserved0;
  ulittle64_t outputHash;
  ulittle64_t outputSize;
  ulittle64_t hardConfigHash;
  ulittle64_t softConfigHash;
  ulittle64_t importTopologyHash;
  ulittle64_t exportTopologyHash;
  ulittle64_t resourceInputHash;
  ulittle64_t sizeOfHeaders;
  ulittle64_t sizeOfImage;
  ulittle64_t outputPathOffset;
  ulittle64_t inputTableOffset;
  ulittle64_t inputCount;
  ulittle64_t sectionTableOffset;
  ulittle64_t sectionCount;
  ulittle64_t chunkTableOffset;
  ulittle64_t chunkCount;
  ulittle64_t symbolTableOffset;
  ulittle64_t symbolCount;
  ulittle64_t envelopeTableOffset;
  ulittle64_t envelopeCount;
  ulittle64_t slotTableOffset;
  ulittle64_t slotCount;
  ulittle64_t packedSectionTableOffset;
  ulittle64_t packedSectionCount;
  ulittle64_t packedKeyTableOffset;
  ulittle64_t packedKeyCount;
  ulittle64_t placementTableOffset;
  ulittle64_t placementCount;
  ulittle64_t edgeTableOffset;
  ulittle64_t edgeCount;
  ulittle64_t redirectTableOffset;
  ulittle64_t redirectCount;
  ulittle64_t poolStateOffset;
  ulittle64_t poolStateCount;
  ulittle64_t stringTableOffset;
  ulittle64_t stringTableSize;
};

struct InputRecord {
  ulittle64_t nameOffset;
  ulittle64_t parentOffset;
  ulittle64_t archiveOffset;
  ulittle64_t contentHash;
  ulittle64_t size;
};

struct SectionRecord {
  ulittle64_t nameOffset;
  ulittle32_t characteristics;
  ulittle32_t reserved;
  ulittle64_t rva;
  ulittle64_t fileOffset;
  ulittle64_t virtualSize;
  ulittle64_t rawSize;
  ulittle32_t firstChunk;
  ulittle32_t chunkCount;
};

struct ChunkRecord {
  ulittle64_t keyOffset;
  ulittle32_t sectionIndex;
  ulittle32_t inputIndex;
  ulittle32_t outputCharacteristics;
  ulittle32_t sectionNumber;
  ulittle32_t alignment;
  ulittle16_t kind;
  ulittle16_t reserved;
  ulittle64_t rva;
  ulittle64_t size;
  ulittle64_t slotCapacity;
  ulittle64_t contentHash;
  ulittle64_t symbolHash;
};

struct SymbolRecord {
  ulittle64_t nameOffset;
  ulittle64_t auxiliaryKeyOffset;
  ulittle32_t inputIndex;
  ulittle16_t kind;
  ulittle16_t reserved;
  ulittle64_t value;
};

struct EnvelopeRecord {
  ulittle64_t nameOffset;
  ulittle32_t characteristics;
  ulittle16_t slotClass;
  ulittle16_t flags;
  ulittle64_t sectionRVA;
  ulittle64_t maxSectionEndRVA;
  ulittle64_t activeEndRVA;
};

struct SlotRecord {
  ulittle64_t occupantKeyOffset;
  ulittle32_t envelopeIndex;
  ulittle32_t minAlignment;
  ulittle16_t state;
  uint8_t fillByte;
  uint8_t reserved0;
  ulittle64_t startRVA;
  ulittle64_t capacity;
  ulittle64_t committedSize;
};

struct PackedSectionRecord {
  ulittle32_t envelopeIndex;
  ulittle32_t firstRecordKey;
  ulittle32_t recordKeyCount;
  ulittle32_t reserved;
  ulittle64_t activePrefixSize;
  ulittle64_t reserveSize;
};

struct PackedKeyRecord {
  ulittle64_t keyOffset;
};

struct PlacementRecord {
  ulittle64_t keyOffset;
  ulittle32_t envelopeIndex;
  ulittle32_t alignment;
  ulittle16_t kind;
  ulittle16_t reserved;
  ulittle64_t startRVA;
  ulittle64_t size;
};

struct EdgeRecord {
  ulittle64_t sourceKeyOffset;
  ulittle64_t targetKeyOffset;
  ulittle32_t sourceOffset;
  ulittle32_t targetOffset;
  ulittle16_t kind;
  uint8_t redirectEligible;
  uint8_t reserved;
};

struct RedirectRecord {
  ulittle64_t targetKeyOffset;
  ulittle64_t canonicalSymbolOffset;
  ulittle64_t redirectRVA;
  ulittle64_t redirectCapacity;
  ulittle64_t bodyRVA;
  ulittle64_t poolThunkRVA;
  uint8_t active;
  uint8_t reserved[7];
};

struct PoolStateRecord {
  ulittle64_t poolStartRVA;
  ulittle64_t poolEndRVA;
  ulittle64_t nextFreeRVA;
};

template <typename T>
Expected<T> readObject(ArrayRef<uint8_t> bytes, uint64_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file is truncated");
  T obj;
  memcpy(&obj, bytes.data() + offset, sizeof(T));
  return obj;
}

template <typename T>
Expected<std::vector<T>> readTable(ArrayRef<uint8_t> bytes, uint64_t offset,
                                   uint64_t count) {
  if (count > std::numeric_limits<size_t>::max())
    return createStringError(inconvertibleErrorCode(),
                             "incremental state table is too large");
  if (offset > bytes.size() || bytes.size() - offset < sizeof(T) * count)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state table is truncated");
  std::vector<T> out(count);
  if (count != 0)
    memcpy(out.data(), bytes.data() + offset, sizeof(T) * count);
  return out;
}

Expected<StringRef> loadString(ArrayRef<uint8_t> strings, uint64_t offset) {
  if (offset >= strings.size())
    return createStringError(inconvertibleErrorCode(),
                             "incremental state string offset is out of range");
  const char *data = reinterpret_cast<const char *>(strings.data() + offset);
  size_t remaining = strings.size() - offset;
  size_t len = strnlen(data, remaining);
  if (len == remaining)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state string is unterminated");
  return StringRef(data, len);
}

class StringTableBuilder {
public:
  StringTableBuilder() { bytes.push_back('\0'); }

  uint64_t add(StringRef s) {
    if (s.empty())
      return 0;
    auto [it, inserted] = offsets.try_emplace(s.str(), bytes.size());
    if (!inserted)
      return it->second;
    bytes.insert(bytes.end(), s.begin(), s.end());
    bytes.push_back('\0');
    return it->second;
  }

  ArrayRef<char> data() const { return bytes; }

private:
  std::vector<char> bytes;
  StringMap<uint64_t> offsets;
};

template <typename T> void appendObject(std::vector<char> &out, const T &obj) {
  size_t oldSize = out.size();
  out.resize(oldSize + sizeof(T));
  memcpy(out.data() + oldSize, &obj, sizeof(T));
}

template <typename HeaderT>
Expected<IncrementalStateFile>
loadCommonState(ArrayRef<uint8_t> bytes, const HeaderT &header,
                IncrementalLayoutMode layoutMode) {
  if (header.stringTableOffset > bytes.size() ||
      header.stringTableSize > bytes.size() - header.stringTableOffset)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state string table is truncated");
  ArrayRef<uint8_t> strings =
      bytes.slice(header.stringTableOffset, header.stringTableSize);

  IncrementalStateFile state;
  state.version = header.version;
  state.layoutMode = layoutMode;
  state.machine =
      static_cast<llvm::COFF::MachineTypes>(uint16_t(header.machine));
  state.outputHash = header.outputHash;
  state.outputSize = header.outputSize;
  state.hardConfigHash = header.hardConfigHash;
  state.softConfigHash = header.softConfigHash;
  state.importTopologyHash = header.importTopologyHash;
  state.exportTopologyHash = header.exportTopologyHash;
  state.resourceInputHash = header.resourceInputHash;
  state.sizeOfHeaders = header.sizeOfHeaders;
  state.sizeOfImage = header.sizeOfImage;

  Expected<StringRef> outputPathOrErr =
      loadString(strings, header.outputPathOffset);
  if (!outputPathOrErr)
    return outputPathOrErr.takeError();
  state.outputPath = outputPathOrErr->str();

  Expected<std::vector<InputRecord>> inputsOrErr =
      readTable<InputRecord>(bytes, header.inputTableOffset, header.inputCount);
  if (!inputsOrErr)
    return inputsOrErr.takeError();
  state.inputs.reserve(inputsOrErr->size());
  for (const InputRecord &record : *inputsOrErr) {
    IncrementalInputState input;
    Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
    if (!nameOrErr)
      return nameOrErr.takeError();
    Expected<StringRef> parentOrErr = loadString(strings, record.parentOffset);
    if (!parentOrErr)
      return parentOrErr.takeError();
    input.name = nameOrErr->str();
    input.parentName = parentOrErr->str();
    input.archiveOffset = record.archiveOffset;
    input.contentHash = record.contentHash;
    input.size = record.size;
    state.inputs.push_back(std::move(input));
  }

  Expected<std::vector<SectionRecord>> sectionsOrErr = readTable<SectionRecord>(
      bytes, header.sectionTableOffset, header.sectionCount);
  if (!sectionsOrErr)
    return sectionsOrErr.takeError();
  state.sections.reserve(sectionsOrErr->size());
  for (const SectionRecord &record : *sectionsOrErr) {
    IncrementalSectionState section;
    Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
    if (!nameOrErr)
      return nameOrErr.takeError();
    section.name = nameOrErr->str();
    section.characteristics = record.characteristics;
    section.rva = record.rva;
    section.fileOffset = record.fileOffset;
    section.virtualSize = record.virtualSize;
    section.rawSize = record.rawSize;
    section.firstChunk = record.firstChunk;
    section.chunkCount = record.chunkCount;
    state.sections.push_back(std::move(section));
  }

  Expected<std::vector<ChunkRecord>> chunksOrErr =
      readTable<ChunkRecord>(bytes, header.chunkTableOffset, header.chunkCount);
  if (!chunksOrErr)
    return chunksOrErr.takeError();
  state.chunks.reserve(chunksOrErr->size());
  for (const ChunkRecord &record : *chunksOrErr) {
    IncrementalChunkState chunk;
    Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();
    chunk.kind = static_cast<IncrementalChunkKind>(uint16_t(record.kind));
    chunk.key = keyOrErr->str();
    chunk.sectionIndex = record.sectionIndex;
    chunk.inputIndex = record.inputIndex;
    chunk.outputCharacteristics = record.outputCharacteristics;
    chunk.sectionNumber = record.sectionNumber;
    chunk.alignment = record.alignment;
    chunk.rva = record.rva;
    chunk.size = record.size;
    chunk.slotCapacity = record.slotCapacity;
    chunk.contentHash = record.contentHash;
    chunk.symbolHash = record.symbolHash;
    state.chunks.push_back(std::move(chunk));
  }

  Expected<std::vector<SymbolRecord>> symbolsOrErr =
      readTable<SymbolRecord>(bytes, header.symbolTableOffset,
                              header.symbolCount);
  if (!symbolsOrErr)
    return symbolsOrErr.takeError();
  state.symbols.reserve(symbolsOrErr->size());
  for (const SymbolRecord &record : *symbolsOrErr) {
    IncrementalSymbolState symbol;
    Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
    if (!nameOrErr)
      return nameOrErr.takeError();
    Expected<StringRef> keyOrErr =
        loadString(strings, record.auxiliaryKeyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();
    symbol.name = nameOrErr->str();
    symbol.auxiliaryKey = keyOrErr->str();
    symbol.kind = static_cast<IncrementalSymbolKind>(uint16_t(record.kind));
    symbol.inputIndex = record.inputIndex;
    symbol.value = record.value;
    state.symbols.push_back(std::move(symbol));
  }

  return state;
}

Expected<IncrementalStateFile> loadIncrementalStateV2(ArrayRef<uint8_t> bytes,
                                                      const FileHeaderV2 &header) {
  return loadCommonState(bytes, header, IncrementalLayoutMode::Exact);
}

Expected<IncrementalStateFile> loadIncrementalStateV3(ArrayRef<uint8_t> bytes,
                                                      const FileHeaderV3 &header) {
  Expected<IncrementalStateFile> stateOrErr =
      loadCommonState(bytes, header,
                      static_cast<IncrementalLayoutMode>(uint16_t(header.layoutMode)));
  if (!stateOrErr)
    return stateOrErr.takeError();

  IncrementalStateFile state = std::move(*stateOrErr);
  if (state.layoutMode != IncrementalLayoutMode::Exact &&
      state.layoutMode != IncrementalLayoutMode::Slotted)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an invalid layout mode");

  if (header.envelopeCount != 0) {
    Expected<std::vector<EnvelopeRecord>> envelopesOrErr = readTable<EnvelopeRecord>(
        bytes, header.envelopeTableOffset, header.envelopeCount);
    if (!envelopesOrErr)
      return envelopesOrErr.takeError();

    ArrayRef<uint8_t> strings =
        bytes.slice(header.stringTableOffset, header.stringTableSize);
    state.sectionEnvelopes.reserve(envelopesOrErr->size());
    for (const EnvelopeRecord &record : *envelopesOrErr) {
      IncrementalSectionEnvelopeState envelope;
      Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
      if (!nameOrErr)
        return nameOrErr.takeError();
      envelope.name = nameOrErr->str();
      envelope.characteristics = record.characteristics;
      envelope.sectionRVA = record.sectionRVA;
      envelope.maxSectionEndRVA = record.maxSectionEndRVA;
      envelope.activeEndRVA = record.activeEndRVA;
      envelope.slotClass =
          static_cast<IncrementalSlotClass>(uint16_t(record.slotClass));
      envelope.packedActivePrefix =
          (record.flags & envelopeFlagPackedActivePrefix) != 0;
      envelope.slotReuseEnabled =
          (record.flags & envelopeFlagSlotReuseEnabled) != 0;
      state.sectionEnvelopes.push_back(std::move(envelope));
    }
  }

  if (header.slotCount != 0) {
    Expected<std::vector<SlotRecord>> slotsOrErr =
        readTable<SlotRecord>(bytes, header.slotTableOffset, header.slotCount);
    if (!slotsOrErr)
      return slotsOrErr.takeError();

    ArrayRef<uint8_t> strings =
        bytes.slice(header.stringTableOffset, header.stringTableSize);
    state.slotRecords.reserve(slotsOrErr->size());
    for (const SlotRecord &record : *slotsOrErr) {
      IncrementalSlotRecordState slot;
      Expected<StringRef> keyOrErr =
          loadString(strings, record.occupantKeyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      slot.envelopeIndex = record.envelopeIndex;
      slot.startRVA = record.startRVA;
      slot.capacity = record.capacity;
      slot.committedSize = record.committedSize;
      slot.minAlignment = record.minAlignment;
      slot.fillByte = record.fillByte;
      slot.state = static_cast<IncrementalSlotState>(uint16_t(record.state));
      slot.occupantKey = keyOrErr->str();
      state.slotRecords.push_back(std::move(slot));
    }
  }

  SmallVector<StringRef, 32> packedKeys;
  if (header.packedKeyCount != 0) {
    Expected<std::vector<PackedKeyRecord>> packedKeysOrErr =
        readTable<PackedKeyRecord>(bytes, header.packedKeyTableOffset,
                                   header.packedKeyCount);
    if (!packedKeysOrErr)
      return packedKeysOrErr.takeError();

    ArrayRef<uint8_t> strings =
        bytes.slice(header.stringTableOffset, header.stringTableSize);
    packedKeys.reserve(packedKeysOrErr->size());
    for (const PackedKeyRecord &record : *packedKeysOrErr) {
      Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      packedKeys.push_back(*keyOrErr);
    }
  }

  if (header.packedSectionCount != 0) {
    Expected<std::vector<PackedSectionRecord>> packedSectionsOrErr =
        readTable<PackedSectionRecord>(bytes, header.packedSectionTableOffset,
                                       header.packedSectionCount);
    if (!packedSectionsOrErr)
      return packedSectionsOrErr.takeError();

    state.packedSections.reserve(packedSectionsOrErr->size());
    for (const PackedSectionRecord &record : *packedSectionsOrErr) {
      if (record.firstRecordKey > packedKeys.size() ||
          packedKeys.size() - record.firstRecordKey < record.recordKeyCount)
        return createStringError(inconvertibleErrorCode(),
                                 "incremental packed section key range is invalid");
      IncrementalPackedSectionState packedSection;
      packedSection.envelopeIndex = record.envelopeIndex;
      packedSection.activePrefixSize = record.activePrefixSize;
      packedSection.reserveSize = record.reserveSize;
      packedSection.recordKeys.reserve(record.recordKeyCount);
      for (StringRef key : ArrayRef<StringRef>(packedKeys).slice(
               record.firstRecordKey, record.recordKeyCount))
        packedSection.recordKeys.push_back(key.str());
      state.packedSections.push_back(std::move(packedSection));
    }
  }

  if (header.placementCount != 0) {
    Expected<std::vector<PlacementRecord>> placementsOrErr =
        readTable<PlacementRecord>(bytes, header.placementTableOffset,
                                   header.placementCount);
    if (!placementsOrErr)
      return placementsOrErr.takeError();

    ArrayRef<uint8_t> strings =
        bytes.slice(header.stringTableOffset, header.stringTableSize);
    state.placements.reserve(placementsOrErr->size());
    for (const PlacementRecord &record : *placementsOrErr) {
      IncrementalPlacementState placement;
      Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      placement.key = keyOrErr->str();
      placement.envelopeIndex = record.envelopeIndex;
      placement.kind =
          static_cast<IncrementalPlacementKind>(uint16_t(record.kind));
      placement.startRVA = record.startRVA;
      placement.size = record.size;
      placement.alignment = record.alignment;
      state.placements.push_back(std::move(placement));
    }
  }

  return state;
}

Expected<IncrementalStateFile> loadIncrementalStateV4(ArrayRef<uint8_t> bytes,
                                                      const FileHeaderV4 &header) {
  Expected<IncrementalStateFile> stateOrErr =
      loadCommonState(bytes, header,
                      static_cast<IncrementalLayoutMode>(
                          uint16_t(header.layoutMode)));
  if (!stateOrErr)
    return stateOrErr.takeError();

  IncrementalStateFile state = std::move(*stateOrErr);
  if (state.layoutMode != IncrementalLayoutMode::Exact &&
      state.layoutMode != IncrementalLayoutMode::Slotted)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an invalid layout mode");

  ArrayRef<uint8_t> strings =
      bytes.slice(header.stringTableOffset, header.stringTableSize);

  if (header.envelopeCount != 0) {
    Expected<std::vector<EnvelopeRecord>> envelopesOrErr =
        readTable<EnvelopeRecord>(bytes, header.envelopeTableOffset,
                                  header.envelopeCount);
    if (!envelopesOrErr)
      return envelopesOrErr.takeError();
    state.sectionEnvelopes.reserve(envelopesOrErr->size());
    for (const EnvelopeRecord &record : *envelopesOrErr) {
      IncrementalSectionEnvelopeState envelope;
      Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
      if (!nameOrErr)
        return nameOrErr.takeError();
      envelope.name = nameOrErr->str();
      envelope.characteristics = record.characteristics;
      envelope.sectionRVA = record.sectionRVA;
      envelope.maxSectionEndRVA = record.maxSectionEndRVA;
      envelope.activeEndRVA = record.activeEndRVA;
      envelope.slotClass =
          static_cast<IncrementalSlotClass>(uint16_t(record.slotClass));
      envelope.packedActivePrefix =
          (record.flags & envelopeFlagPackedActivePrefix) != 0;
      envelope.slotReuseEnabled =
          (record.flags & envelopeFlagSlotReuseEnabled) != 0;
      state.sectionEnvelopes.push_back(std::move(envelope));
    }
  }

  if (header.slotCount != 0) {
    Expected<std::vector<SlotRecord>> slotsOrErr =
        readTable<SlotRecord>(bytes, header.slotTableOffset, header.slotCount);
    if (!slotsOrErr)
      return slotsOrErr.takeError();
    state.slotRecords.reserve(slotsOrErr->size());
    for (const SlotRecord &record : *slotsOrErr) {
      IncrementalSlotRecordState slot;
      Expected<StringRef> keyOrErr =
          loadString(strings, record.occupantKeyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      slot.envelopeIndex = record.envelopeIndex;
      slot.startRVA = record.startRVA;
      slot.capacity = record.capacity;
      slot.committedSize = record.committedSize;
      slot.minAlignment = record.minAlignment;
      slot.fillByte = record.fillByte;
      slot.state = static_cast<IncrementalSlotState>(uint16_t(record.state));
      slot.occupantKey = keyOrErr->str();
      state.slotRecords.push_back(std::move(slot));
    }
  }

  SmallVector<StringRef, 32> packedKeys;
  if (header.packedKeyCount != 0) {
    Expected<std::vector<PackedKeyRecord>> packedKeysOrErr =
        readTable<PackedKeyRecord>(bytes, header.packedKeyTableOffset,
                                   header.packedKeyCount);
    if (!packedKeysOrErr)
      return packedKeysOrErr.takeError();
    packedKeys.reserve(packedKeysOrErr->size());
    for (const PackedKeyRecord &record : *packedKeysOrErr) {
      Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      packedKeys.push_back(*keyOrErr);
    }
  }

  if (header.packedSectionCount != 0) {
    Expected<std::vector<PackedSectionRecord>> packedSectionsOrErr =
        readTable<PackedSectionRecord>(bytes, header.packedSectionTableOffset,
                                       header.packedSectionCount);
    if (!packedSectionsOrErr)
      return packedSectionsOrErr.takeError();
    state.packedSections.reserve(packedSectionsOrErr->size());
    for (const PackedSectionRecord &record : *packedSectionsOrErr) {
      if (record.firstRecordKey > packedKeys.size() ||
          packedKeys.size() - record.firstRecordKey < record.recordKeyCount)
        return createStringError(inconvertibleErrorCode(),
                                 "incremental packed section key range is invalid");
      IncrementalPackedSectionState packedSection;
      packedSection.envelopeIndex = record.envelopeIndex;
      packedSection.activePrefixSize = record.activePrefixSize;
      packedSection.reserveSize = record.reserveSize;
      packedSection.recordKeys.reserve(record.recordKeyCount);
      for (StringRef key : ArrayRef<StringRef>(packedKeys).slice(
               record.firstRecordKey, record.recordKeyCount))
        packedSection.recordKeys.push_back(key.str());
      state.packedSections.push_back(std::move(packedSection));
    }
  }

  if (header.placementCount != 0) {
    Expected<std::vector<PlacementRecord>> placementsOrErr =
        readTable<PlacementRecord>(bytes, header.placementTableOffset,
                                   header.placementCount);
    if (!placementsOrErr)
      return placementsOrErr.takeError();
    state.placements.reserve(placementsOrErr->size());
    for (const PlacementRecord &record : *placementsOrErr) {
      IncrementalPlacementState placement;
      Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      placement.key = keyOrErr->str();
      placement.envelopeIndex = record.envelopeIndex;
      placement.kind =
          static_cast<IncrementalPlacementKind>(uint16_t(record.kind));
      placement.startRVA = record.startRVA;
      placement.size = record.size;
      placement.alignment = record.alignment;
      state.placements.push_back(std::move(placement));
    }
  }

  if (header.edgeCount != 0) {
    Expected<std::vector<EdgeRecord>> edgesOrErr =
        readTable<EdgeRecord>(bytes, header.edgeTableOffset, header.edgeCount);
    if (!edgesOrErr)
      return edgesOrErr.takeError();
    state.edges.reserve(edgesOrErr->size());
    for (const EdgeRecord &record : *edgesOrErr) {
      IncrementalEdgeState edge;
      Expected<StringRef> sourceOrErr = loadString(strings, record.sourceKeyOffset);
      if (!sourceOrErr)
        return sourceOrErr.takeError();
      Expected<StringRef> targetOrErr = loadString(strings, record.targetKeyOffset);
      if (!targetOrErr)
        return targetOrErr.takeError();
      edge.sourceKey = sourceOrErr->str();
      edge.targetKey = targetOrErr->str();
      edge.kind = static_cast<IncrementalRefKind>(uint16_t(record.kind));
      edge.sourceOffset = record.sourceOffset;
      edge.targetOffset = record.targetOffset;
      edge.redirectEligible = record.redirectEligible != 0;
      state.edges.push_back(std::move(edge));
    }
  }

  if (header.redirectCount != 0) {
    Expected<std::vector<RedirectRecord>> redirectsOrErr = readTable<RedirectRecord>(
        bytes, header.redirectTableOffset, header.redirectCount);
    if (!redirectsOrErr)
      return redirectsOrErr.takeError();
    state.textRedirects.reserve(redirectsOrErr->size());
    for (const RedirectRecord &record : *redirectsOrErr) {
      IncrementalTextRedirectState redirect;
      Expected<StringRef> keyOrErr = loadString(strings, record.targetKeyOffset);
      if (!keyOrErr)
        return keyOrErr.takeError();
      Expected<StringRef> symbolOrErr =
          loadString(strings, record.canonicalSymbolOffset);
      if (!symbolOrErr)
        return symbolOrErr.takeError();
      redirect.targetKey = keyOrErr->str();
      redirect.canonicalSymbol = symbolOrErr->str();
      redirect.redirectRVA = record.redirectRVA;
      redirect.redirectCapacity = record.redirectCapacity;
      redirect.bodyRVA = record.bodyRVA;
      redirect.poolThunkRVA = record.poolThunkRVA;
      redirect.active = record.active != 0;
      state.textRedirects.push_back(std::move(redirect));
    }
  }

  if (header.poolStateCount != 0) {
    Expected<std::vector<PoolStateRecord>> poolOrErr = readTable<PoolStateRecord>(
        bytes, header.poolStateOffset, header.poolStateCount);
    if (!poolOrErr)
      return poolOrErr.takeError();
    if (!poolOrErr->empty()) {
      state.textThunkPool.poolStartRVA = (*poolOrErr)[0].poolStartRVA;
      state.textThunkPool.poolEndRVA = (*poolOrErr)[0].poolEndRVA;
      state.textThunkPool.nextFreeRVA = (*poolOrErr)[0].nextFreeRVA;
    }
  }

  return state;
}

} // namespace

Expected<IncrementalStateFile> loadIncrementalState(StringRef path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!buffer)
    return createFileError(path, errorCodeToError(buffer.getError()));

  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(
                              (*buffer)->getBufferStart()),
                          (*buffer)->getBufferSize());
  Expected<FileHeaderV2> prefixOrErr = readObject<FileHeaderV2>(bytes, 0);
  if (!prefixOrErr)
    return prefixOrErr.takeError();

  if (memcmp(prefixOrErr->magic, stateMagic, sizeof(stateMagic)) != 0)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an invalid magic");

  switch (uint32_t(prefixOrErr->version)) {
  case exactStateVersion:
    return loadIncrementalStateV2(bytes, *prefixOrErr);
  case slottedStateVersion: {
    Expected<FileHeaderV3> headerOrErr = readObject<FileHeaderV3>(bytes, 0);
    if (!headerOrErr)
      return headerOrErr.takeError();
    return loadIncrementalStateV3(bytes, *headerOrErr);
  }
  case redirectStateVersion: {
    Expected<FileHeaderV4> headerOrErr = readObject<FileHeaderV4>(bytes, 0);
    if (!headerOrErr)
      return headerOrErr.takeError();
    return loadIncrementalStateV4(bytes, *headerOrErr);
  }
  default:
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an unsupported version");
  }
}

Error writeIncrementalState(StringRef path, const IncrementalStateFile &state) {
  FileHeaderV4 header = {};
  memcpy(header.magic, stateMagic, sizeof(stateMagic));
  header.version = redirectStateVersion;
  header.machine = state.machine;
  header.flags = 0;
  header.layoutMode = static_cast<uint16_t>(state.layoutMode);
  header.outputHash = state.outputHash;
  header.outputSize = state.outputSize;
  header.hardConfigHash = state.hardConfigHash;
  header.softConfigHash = state.softConfigHash;
  header.importTopologyHash = state.importTopologyHash;
  header.exportTopologyHash = state.exportTopologyHash;
  header.resourceInputHash = state.resourceInputHash;
  header.sizeOfHeaders = state.sizeOfHeaders;
  header.sizeOfImage = state.sizeOfImage;

  StringTableBuilder strings;
  std::vector<InputRecord> inputRecords;
  std::vector<SectionRecord> sectionRecords;
  std::vector<ChunkRecord> chunkRecords;
  std::vector<SymbolRecord> symbolRecords;
  std::vector<EnvelopeRecord> envelopeRecords;
  std::vector<SlotRecord> slotRecords;
  std::vector<PackedSectionRecord> packedSectionRecords;
  std::vector<PackedKeyRecord> packedKeyRecords;
  std::vector<PlacementRecord> placementRecords;
  std::vector<EdgeRecord> edgeRecords;
  std::vector<RedirectRecord> redirectRecords;
  std::vector<PoolStateRecord> poolStateRecords;

  header.outputPathOffset = strings.add(state.outputPath);

  inputRecords.reserve(state.inputs.size());
  for (const IncrementalInputState &input : state.inputs) {
    InputRecord record = {};
    record.nameOffset = strings.add(input.name);
    record.parentOffset = strings.add(input.parentName);
    record.archiveOffset = input.archiveOffset;
    record.contentHash = input.contentHash;
    record.size = input.size;
    inputRecords.push_back(record);
  }

  sectionRecords.reserve(state.sections.size());
  for (const IncrementalSectionState &section : state.sections) {
    SectionRecord record = {};
    record.nameOffset = strings.add(section.name);
    record.characteristics = section.characteristics;
    record.rva = section.rva;
    record.fileOffset = section.fileOffset;
    record.virtualSize = section.virtualSize;
    record.rawSize = section.rawSize;
    record.firstChunk = section.firstChunk;
    record.chunkCount = section.chunkCount;
    sectionRecords.push_back(record);
  }

  chunkRecords.reserve(state.chunks.size());
  for (const IncrementalChunkState &chunk : state.chunks) {
    ChunkRecord record = {};
    record.keyOffset = strings.add(chunk.key);
    record.sectionIndex = chunk.sectionIndex;
    record.inputIndex = chunk.inputIndex;
    record.outputCharacteristics = chunk.outputCharacteristics;
    record.sectionNumber = chunk.sectionNumber;
    record.alignment = chunk.alignment;
    record.kind = static_cast<uint16_t>(chunk.kind);
    record.rva = chunk.rva;
    record.size = chunk.size;
    record.slotCapacity = chunk.slotCapacity;
    record.contentHash = chunk.contentHash;
    record.symbolHash = chunk.symbolHash;
    chunkRecords.push_back(record);
  }

  symbolRecords.reserve(state.symbols.size());
  for (const IncrementalSymbolState &symbol : state.symbols) {
    SymbolRecord record = {};
    record.nameOffset = strings.add(symbol.name);
    record.auxiliaryKeyOffset = strings.add(symbol.auxiliaryKey);
    record.inputIndex = symbol.inputIndex;
    record.kind = static_cast<uint16_t>(symbol.kind);
    record.value = symbol.value;
    symbolRecords.push_back(record);
  }

  envelopeRecords.reserve(state.sectionEnvelopes.size());
  for (const IncrementalSectionEnvelopeState &envelope : state.sectionEnvelopes) {
    EnvelopeRecord record = {};
    record.nameOffset = strings.add(envelope.name);
    record.characteristics = envelope.characteristics;
    record.slotClass = static_cast<uint16_t>(envelope.slotClass);
    if (envelope.packedActivePrefix)
      record.flags |= envelopeFlagPackedActivePrefix;
    if (envelope.slotReuseEnabled)
      record.flags |= envelopeFlagSlotReuseEnabled;
    record.sectionRVA = envelope.sectionRVA;
    record.maxSectionEndRVA = envelope.maxSectionEndRVA;
    record.activeEndRVA = envelope.activeEndRVA;
    envelopeRecords.push_back(record);
  }

  slotRecords.reserve(state.slotRecords.size());
  for (const IncrementalSlotRecordState &slot : state.slotRecords) {
    SlotRecord record = {};
    record.occupantKeyOffset = strings.add(slot.occupantKey);
    record.envelopeIndex = slot.envelopeIndex;
    record.minAlignment = slot.minAlignment;
    record.state = static_cast<uint16_t>(slot.state);
    record.fillByte = slot.fillByte;
    record.startRVA = slot.startRVA;
    record.capacity = slot.capacity;
    record.committedSize = slot.committedSize;
    slotRecords.push_back(record);
  }

  packedSectionRecords.reserve(state.packedSections.size());
  for (const IncrementalPackedSectionState &packedSection : state.packedSections) {
    PackedSectionRecord record = {};
    record.envelopeIndex = packedSection.envelopeIndex;
    record.firstRecordKey = packedKeyRecords.size();
    record.recordKeyCount = packedSection.recordKeys.size();
    record.activePrefixSize = packedSection.activePrefixSize;
    record.reserveSize = packedSection.reserveSize;
    for (StringRef key : packedSection.recordKeys) {
      PackedKeyRecord keyRecord = {};
      keyRecord.keyOffset = strings.add(key);
      packedKeyRecords.push_back(keyRecord);
    }
    packedSectionRecords.push_back(record);
  }

  placementRecords.reserve(state.placements.size());
  for (const IncrementalPlacementState &placement : state.placements) {
    PlacementRecord record = {};
    record.keyOffset = strings.add(placement.key);
    record.envelopeIndex = placement.envelopeIndex;
    record.alignment = placement.alignment;
    record.kind = static_cast<uint16_t>(placement.kind);
    record.startRVA = placement.startRVA;
    record.size = placement.size;
    placementRecords.push_back(record);
  }

  edgeRecords.reserve(state.edges.size());
  for (const IncrementalEdgeState &edge : state.edges) {
    EdgeRecord record = {};
    record.sourceKeyOffset = strings.add(edge.sourceKey);
    record.targetKeyOffset = strings.add(edge.targetKey);
    record.sourceOffset = edge.sourceOffset;
    record.targetOffset = edge.targetOffset;
    record.kind = static_cast<uint16_t>(edge.kind);
    record.redirectEligible = edge.redirectEligible ? 1 : 0;
    edgeRecords.push_back(record);
  }

  redirectRecords.reserve(state.textRedirects.size());
  for (const IncrementalTextRedirectState &redirect : state.textRedirects) {
    RedirectRecord record = {};
    record.targetKeyOffset = strings.add(redirect.targetKey);
    record.canonicalSymbolOffset = strings.add(redirect.canonicalSymbol);
    record.redirectRVA = redirect.redirectRVA;
    record.redirectCapacity = redirect.redirectCapacity;
    record.bodyRVA = redirect.bodyRVA;
    record.poolThunkRVA = redirect.poolThunkRVA;
    record.active = redirect.active ? 1 : 0;
    redirectRecords.push_back(record);
  }

  if (state.textThunkPool.poolStartRVA != 0 || state.textThunkPool.poolEndRVA != 0 ||
      state.textThunkPool.nextFreeRVA != 0) {
    PoolStateRecord record = {};
    record.poolStartRVA = state.textThunkPool.poolStartRVA;
    record.poolEndRVA = state.textThunkPool.poolEndRVA;
    record.nextFreeRVA = state.textThunkPool.nextFreeRVA;
    poolStateRecords.push_back(record);
  }

  header.inputTableOffset = sizeof(FileHeaderV4);
  header.inputCount = inputRecords.size();
  header.sectionTableOffset =
      header.inputTableOffset + inputRecords.size() * sizeof(InputRecord);
  header.sectionCount = sectionRecords.size();
  header.chunkTableOffset =
      header.sectionTableOffset + sectionRecords.size() * sizeof(SectionRecord);
  header.chunkCount = chunkRecords.size();
  header.symbolTableOffset =
      header.chunkTableOffset + chunkRecords.size() * sizeof(ChunkRecord);
  header.symbolCount = symbolRecords.size();
  header.envelopeTableOffset =
      header.symbolTableOffset + symbolRecords.size() * sizeof(SymbolRecord);
  header.envelopeCount = envelopeRecords.size();
  header.slotTableOffset =
      header.envelopeTableOffset + envelopeRecords.size() * sizeof(EnvelopeRecord);
  header.slotCount = slotRecords.size();
  header.packedSectionTableOffset =
      header.slotTableOffset + slotRecords.size() * sizeof(SlotRecord);
  header.packedSectionCount = packedSectionRecords.size();
  header.packedKeyTableOffset =
      header.packedSectionTableOffset +
      packedSectionRecords.size() * sizeof(PackedSectionRecord);
  header.packedKeyCount = packedKeyRecords.size();
  header.placementTableOffset =
      header.packedKeyTableOffset + packedKeyRecords.size() * sizeof(PackedKeyRecord);
  header.placementCount = placementRecords.size();
  header.edgeTableOffset =
      header.placementTableOffset + placementRecords.size() * sizeof(PlacementRecord);
  header.edgeCount = edgeRecords.size();
  header.redirectTableOffset =
      header.edgeTableOffset + edgeRecords.size() * sizeof(EdgeRecord);
  header.redirectCount = redirectRecords.size();
  header.poolStateOffset =
      header.redirectTableOffset + redirectRecords.size() * sizeof(RedirectRecord);
  header.poolStateCount = poolStateRecords.size();
  header.stringTableOffset =
      header.poolStateOffset + poolStateRecords.size() * sizeof(PoolStateRecord);
  header.stringTableSize = strings.data().size();

  std::vector<char> buffer;
  buffer.reserve(header.stringTableOffset + header.stringTableSize);
  appendObject(buffer, header);
  for (const InputRecord &record : inputRecords)
    appendObject(buffer, record);
  for (const SectionRecord &record : sectionRecords)
    appendObject(buffer, record);
  for (const ChunkRecord &record : chunkRecords)
    appendObject(buffer, record);
  for (const SymbolRecord &record : symbolRecords)
    appendObject(buffer, record);
  for (const EnvelopeRecord &record : envelopeRecords)
    appendObject(buffer, record);
  for (const SlotRecord &record : slotRecords)
    appendObject(buffer, record);
  for (const PackedSectionRecord &record : packedSectionRecords)
    appendObject(buffer, record);
  for (const PackedKeyRecord &record : packedKeyRecords)
    appendObject(buffer, record);
  for (const PlacementRecord &record : placementRecords)
    appendObject(buffer, record);
  for (const EdgeRecord &record : edgeRecords)
    appendObject(buffer, record);
  for (const RedirectRecord &record : redirectRecords)
    appendObject(buffer, record);
  for (const PoolStateRecord &record : poolStateRecords)
    appendObject(buffer, record);
  buffer.insert(buffer.end(), strings.data().begin(), strings.data().end());

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
  os.write(buffer.data(), buffer.size());
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

} // namespace lld::coff
