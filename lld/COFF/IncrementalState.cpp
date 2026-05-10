#include "IncrementalState.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>
#include <limits>

using namespace llvm;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {

namespace {

constexpr char stateMagic[8] = {'L', 'L', 'I', 'L', 'K', '6', '4', '\0'};
constexpr uint32_t currentStateVersion = 8;

enum class WireIncrementalLayoutMode : uint16_t {
  Exact = 1,
  Slotted = 2,
};

enum class WireIncrementalChunkKind : uint16_t {
  ObjSection = 1,
  Synthetic = 2,
  Padding = 3,
  EntryRedirect = 4,
  LongThunk = 5,
};

enum class WireIncrementalSymbolKind : uint16_t {
  Regular = 1,
  Common = 2,
  ImportData = 3,
  ImportThunk = 4,
  LocalImport = 5,
  Absolute = 6,
  Synthetic = 7,
};

enum class WireIncrementalSectionLayoutKind : uint16_t {
  ExactSectionLayout = 1,
  TextFreeSlots = 2,
  ReadOnlyDataFreeSlots = 3,
  WritableDataFreeSlots = 4,
  PackedPDataPrefix = 5,
  PackedXDataPrefix = 6,
};

enum class WireIncrementalSlotState : uint16_t {
  Occupied = 1,
  Free = 2,
};

enum class WireIncrementalPlacementKind : uint16_t {
  ExistingSlot = 1,
  ReusedFreeSlot = 2,
  TailReserve = 3,
  PackedPrefix = 4,
};

struct FileHeaderV5 {
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
  ulittle16_t layoutKind;
  ulittle16_t reserved;
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

struct RedirectRecord {
  ulittle64_t targetKeyOffset;
  ulittle64_t canonicalSymbolOffset;
  ulittle64_t redirectRVA;
  ulittle64_t redirectCapacity;
  ulittle64_t bodyRVA;
  ulittle64_t poolThunkRVA;
  ulittle64_t reserved;
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

static bool isValidWireChunkKind(uint16_t rawKind) {
  switch (static_cast<WireIncrementalChunkKind>(rawKind)) {
  case WireIncrementalChunkKind::ObjSection:
  case WireIncrementalChunkKind::Synthetic:
  case WireIncrementalChunkKind::Padding:
  case WireIncrementalChunkKind::EntryRedirect:
  case WireIncrementalChunkKind::LongThunk:
    return true;
  }
  return false;
}

static bool isValidWireSymbolKind(uint16_t rawKind) {
  switch (static_cast<WireIncrementalSymbolKind>(rawKind)) {
  case WireIncrementalSymbolKind::Regular:
  case WireIncrementalSymbolKind::Common:
  case WireIncrementalSymbolKind::ImportData:
  case WireIncrementalSymbolKind::ImportThunk:
  case WireIncrementalSymbolKind::LocalImport:
  case WireIncrementalSymbolKind::Absolute:
  case WireIncrementalSymbolKind::Synthetic:
    return true;
  }
  return false;
}

static bool isValidWireSectionLayoutKind(uint16_t rawKind) {
  switch (static_cast<WireIncrementalSectionLayoutKind>(rawKind)) {
  case WireIncrementalSectionLayoutKind::ExactSectionLayout:
  case WireIncrementalSectionLayoutKind::TextFreeSlots:
  case WireIncrementalSectionLayoutKind::ReadOnlyDataFreeSlots:
  case WireIncrementalSectionLayoutKind::WritableDataFreeSlots:
  case WireIncrementalSectionLayoutKind::PackedPDataPrefix:
  case WireIncrementalSectionLayoutKind::PackedXDataPrefix:
    return true;
  }
  return false;
}

static bool isValidWireSlotState(uint16_t rawState) {
  switch (static_cast<WireIncrementalSlotState>(rawState)) {
  case WireIncrementalSlotState::Occupied:
  case WireIncrementalSlotState::Free:
    return true;
  }
  return false;
}

static bool isValidWirePlacementKind(uint16_t rawKind) {
  switch (static_cast<WireIncrementalPlacementKind>(rawKind)) {
  case WireIncrementalPlacementKind::ExistingSlot:
  case WireIncrementalPlacementKind::ReusedFreeSlot:
  case WireIncrementalPlacementKind::TailReserve:
  case WireIncrementalPlacementKind::PackedPrefix:
    return true;
  }
  return false;
}

static Expected<uint32_t> parseAlignment(StringRef auxiliaryKey,
                                         StringRef context) {
  uint32_t alignment = 0;
  if (!to_integer(auxiliaryKey, alignment))
    return createStringError(inconvertibleErrorCode(),
                             "incremental state " + context +
                                 " alignment is malformed");
  return alignment;
}

static Expected<ImportDataResolvedSymbol>
decodeImportDataSymbol(StringRef name, StringRef auxiliaryKey, uint64_t value) {
  SmallVector<StringRef, 4> parts;
  auxiliaryKey.split(parts, '\n');
  if (parts.size() != 3)
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state import-data symbol payload is malformed");
  unsigned typeInfo = 0;
  if (!to_integer(parts[2], typeInfo) || typeInfo > UINT16_MAX)
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state import-data type info is malformed");
  ImportDataResolvedSymbol symbol;
  symbol.name = name.str();
  symbol.ordinal = value;
  symbol.dllName = parts[0].str();
  symbol.externalName = parts[1].str();
  symbol.typeInfo = static_cast<uint16_t>(typeInfo);
  return symbol;
}

static std::string
encodeImportDataSymbol(const ImportDataResolvedSymbol &symbol) {
  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  os << symbol.dllName << '\n'
     << symbol.externalName << '\n'
     << symbol.typeInfo;
  return std::string(buffer);
}

static Expected<IncrementalResolvedSymbolSnapshot>
decodeResolvedSymbol(const SymbolRecord &record, StringRef name,
                     StringRef auxiliaryKey) {
  if (!isValidWireSymbolKind(uint16_t(record.kind)))
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state file has an invalid symbol kind");

  switch (static_cast<WireIncrementalSymbolKind>(uint16_t(record.kind))) {
  case WireIncrementalSymbolKind::Regular: {
    if (record.inputIndex != UINT32_MAX) {
      if (auxiliaryKey.empty())
        return createStringError(inconvertibleErrorCode(),
                                 "incremental state object-file regular symbol "
                                 "is missing its chunk key");
      ObjFileRegularResolvedSymbol symbol{name.str(), record.inputIndex,
                                          record.value, auxiliaryKey.str()};
      return IncrementalResolvedSymbolSnapshot::make<
          ObjFileRegularResolvedSymbol>(std::move(symbol));
    }
    if (!auxiliaryKey.empty())
      return createStringError(inconvertibleErrorCode(),
                               "incremental state bitcode regular symbol has "
                               "an unexpected chunk key");
    BitcodeRegularResolvedSymbol symbol{name.str(), record.value};
    return IncrementalResolvedSymbolSnapshot::make<
        BitcodeRegularResolvedSymbol>(std::move(symbol));
  }
  case WireIncrementalSymbolKind::Common: {
    Expected<uint32_t> alignmentOrErr =
        parseAlignment(auxiliaryKey, "common-symbol");
    if (!alignmentOrErr)
      return alignmentOrErr.takeError();
    if (record.inputIndex != UINT32_MAX) {
      ObjFileCommonResolvedSymbol symbol{name.str(), record.inputIndex,
                                         record.value, *alignmentOrErr};
      return IncrementalResolvedSymbolSnapshot::make<
          ObjFileCommonResolvedSymbol>(std::move(symbol));
    }
    BitcodeCommonResolvedSymbol symbol{name.str(), record.value,
                                       *alignmentOrErr};
    return IncrementalResolvedSymbolSnapshot::make<BitcodeCommonResolvedSymbol>(
        std::move(symbol));
  }
  case WireIncrementalSymbolKind::ImportData: {
    Expected<ImportDataResolvedSymbol> symbolOrErr =
        decodeImportDataSymbol(name, auxiliaryKey, record.value);
    if (!symbolOrErr)
      return symbolOrErr.takeError();
    return IncrementalResolvedSymbolSnapshot::make<ImportDataResolvedSymbol>(
        std::move(*symbolOrErr));
  }
  case WireIncrementalSymbolKind::ImportThunk: {
    ImportThunkResolvedSymbol symbol;
    symbol.name = name.str();
    symbol.wrappedSymbolName = auxiliaryKey.str();
    return IncrementalResolvedSymbolSnapshot::make<ImportThunkResolvedSymbol>(
        std::move(symbol));
  }
  case WireIncrementalSymbolKind::LocalImport: {
    if (auxiliaryKey.empty())
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state local-import symbol is missing its chunk key");
    LocalImportResolvedSymbol symbol{name.str(), auxiliaryKey.str()};
    return IncrementalResolvedSymbolSnapshot::make<LocalImportResolvedSymbol>(
        std::move(symbol));
  }
  case WireIncrementalSymbolKind::Absolute: {
    AbsoluteResolvedSymbol symbol;
    symbol.name = name.str();
    symbol.value = record.value;
    return IncrementalResolvedSymbolSnapshot::make<AbsoluteResolvedSymbol>(
        std::move(symbol));
  }
  case WireIncrementalSymbolKind::Synthetic: {
    if (auxiliaryKey.empty()) {
      ImageBaseSyntheticResolvedSymbol symbol{name.str()};
      return IncrementalResolvedSymbolSnapshot::make<
          ImageBaseSyntheticResolvedSymbol>(std::move(symbol));
    }
    ChunkBackedSyntheticResolvedSymbol symbol{name.str(), auxiliaryKey.str()};
    return IncrementalResolvedSymbolSnapshot::make<
        ChunkBackedSyntheticResolvedSymbol>(std::move(symbol));
  }
  }
  llvm_unreachable("invalid wire symbol kind");
}

static void
encodeResolvedSymbol(const IncrementalResolvedSymbolSnapshot &symbol,
                     SymbolRecord &record, StringTableBuilder &strings) {
  symbol.match(
      [&](const ObjFileRegularResolvedSymbol &regular) {
        record.nameOffset = strings.add(regular.name);
        record.auxiliaryKeyOffset = strings.add(regular.chunkKey);
        record.inputIndex = regular.inputIndex;
        record.kind = static_cast<uint16_t>(WireIncrementalSymbolKind::Regular);
        record.value = regular.value;
      },
      [&](const BitcodeRegularResolvedSymbol &regular) {
        record.nameOffset = strings.add(regular.name);
        record.auxiliaryKeyOffset = 0;
        record.inputIndex = UINT32_MAX;
        record.kind = static_cast<uint16_t>(WireIncrementalSymbolKind::Regular);
        record.value = regular.value;
      },
      [&](const ObjFileCommonResolvedSymbol &common) {
        record.nameOffset = strings.add(common.name);
        record.auxiliaryKeyOffset =
            strings.add(std::to_string(common.alignment));
        record.inputIndex = common.inputIndex;
        record.kind = static_cast<uint16_t>(WireIncrementalSymbolKind::Common);
        record.value = common.size;
      },
      [&](const BitcodeCommonResolvedSymbol &common) {
        record.nameOffset = strings.add(common.name);
        record.auxiliaryKeyOffset =
            strings.add(std::to_string(common.alignment));
        record.inputIndex = UINT32_MAX;
        record.kind = static_cast<uint16_t>(WireIncrementalSymbolKind::Common);
        record.value = common.size;
      },
      [&](const ImportDataResolvedSymbol &importData) {
        record.nameOffset = strings.add(importData.name);
        record.auxiliaryKeyOffset =
            strings.add(encodeImportDataSymbol(importData));
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::ImportData);
        record.value = importData.ordinal;
      },
      [&](const ImportThunkResolvedSymbol &importThunk) {
        record.nameOffset = strings.add(importThunk.name);
        record.auxiliaryKeyOffset = strings.add(importThunk.wrappedSymbolName);
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::ImportThunk);
        record.value = 0;
      },
      [&](const LocalImportResolvedSymbol &localImport) {
        record.nameOffset = strings.add(localImport.name);
        record.auxiliaryKeyOffset = strings.add(localImport.chunkKey);
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::LocalImport);
        record.value = 0;
      },
      [&](const AbsoluteResolvedSymbol &absolute) {
        record.nameOffset = strings.add(absolute.name);
        record.auxiliaryKeyOffset = 0;
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::Absolute);
        record.value = absolute.value;
      },
      [&](const ChunkBackedSyntheticResolvedSymbol &synthetic) {
        record.nameOffset = strings.add(synthetic.name);
        record.auxiliaryKeyOffset = strings.add(synthetic.chunkKey);
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::Synthetic);
        record.value = 0;
      },
      [&](const ImageBaseSyntheticResolvedSymbol &synthetic) {
        record.nameOffset = strings.add(synthetic.name);
        record.auxiliaryKeyOffset = 0;
        record.inputIndex = UINT32_MAX;
        record.kind =
            static_cast<uint16_t>(WireIncrementalSymbolKind::Synthetic);
        record.value = 0;
      });
}

static Expected<IncrementalChunkSnapshot>
decodeChunkSnapshot(const ChunkRecord &record, StringRef key) {
  if (!isValidWireChunkKind(uint16_t(record.kind)))
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state file has an invalid chunk kind");

  IncrementalChunkState chunk;
  chunk.key = key.str();
  chunk.sectionIndex = record.sectionIndex;
  chunk.outputCharacteristics = record.outputCharacteristics;
  chunk.alignment = record.alignment;
  chunk.rva = record.rva;
  chunk.size = record.size;
  chunk.slotCapacity = record.slotCapacity;

  switch (static_cast<WireIncrementalChunkKind>(uint16_t(record.kind))) {
  case WireIncrementalChunkKind::ObjSection: {
    if (record.inputIndex == UINT32_MAX)
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state obj-section chunk is missing its input index");
    ObjSectionChunkSnapshot obj;
    obj.chunk = std::move(chunk);
    obj.inputIndex = record.inputIndex;
    obj.sectionNumber = record.sectionNumber;
    obj.contentHash = record.contentHash;
    obj.symbolHash = record.symbolHash;
    return IncrementalChunkSnapshot::make<ObjSectionChunkSnapshot>(
        std::move(obj));
  }
  case WireIncrementalChunkKind::Synthetic:
    return IncrementalChunkSnapshot::make<SyntheticChunkSnapshot>(
        SyntheticChunkSnapshot{std::move(chunk)});
  case WireIncrementalChunkKind::Padding:
    return IncrementalChunkSnapshot::make<PaddingChunkSnapshot>(
        PaddingChunkSnapshot{std::move(chunk)});
  case WireIncrementalChunkKind::EntryRedirect:
    return IncrementalChunkSnapshot::make<EntryRedirectChunkSnapshot>(
        EntryRedirectChunkSnapshot{std::move(chunk)});
  case WireIncrementalChunkKind::LongThunk:
    return IncrementalChunkSnapshot::make<LongThunkChunkSnapshot>(
        LongThunkChunkSnapshot{std::move(chunk)});
  }
  llvm_unreachable("invalid wire chunk kind");
}

static void encodeChunkSnapshot(const IncrementalChunkSnapshot &chunk,
                                ChunkRecord &record,
                                StringTableBuilder &strings) {
  const IncrementalChunkState &common = getIncrementalChunkState(chunk);
  record.keyOffset = strings.add(common.key);
  record.sectionIndex = common.sectionIndex;
  record.inputIndex = UINT32_MAX;
  record.outputCharacteristics = common.outputCharacteristics;
  record.sectionNumber = 0;
  record.alignment = common.alignment;
  record.rva = common.rva;
  record.size = common.size;
  record.slotCapacity = common.slotCapacity;
  record.contentHash = 0;
  record.symbolHash = 0;

  chunk.match(
      [&](const ObjSectionChunkSnapshot &obj) {
        record.kind =
            static_cast<uint16_t>(WireIncrementalChunkKind::ObjSection);
        record.inputIndex = obj.inputIndex;
        record.sectionNumber = obj.sectionNumber;
        record.contentHash = obj.contentHash;
        record.symbolHash = obj.symbolHash;
      },
      [&](const SyntheticChunkSnapshot &) {
        record.kind =
            static_cast<uint16_t>(WireIncrementalChunkKind::Synthetic);
      },
      [&](const PaddingChunkSnapshot &) {
        record.kind = static_cast<uint16_t>(WireIncrementalChunkKind::Padding);
      },
      [&](const EntryRedirectChunkSnapshot &) {
        record.kind =
            static_cast<uint16_t>(WireIncrementalChunkKind::EntryRedirect);
      },
      [&](const LongThunkChunkSnapshot &) {
        record.kind =
            static_cast<uint16_t>(WireIncrementalChunkKind::LongThunk);
      });
}

struct RawEnvelopeData {
  IncrementalSectionState section;
  WireIncrementalSectionLayoutKind layoutKind;
  uint64_t maxSectionEndRVA = 0;
  uint64_t activeEndRVA = 0;
};

static Expected<std::vector<IncrementalInputState>>
decodeInputs(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
             ArrayRef<uint8_t> strings) {
  Expected<std::vector<InputRecord>> inputsOrErr =
      readTable<InputRecord>(bytes, header.inputTableOffset, header.inputCount);
  if (!inputsOrErr)
    return inputsOrErr.takeError();

  std::vector<IncrementalInputState> inputs;
  inputs.reserve(inputsOrErr->size());
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
    inputs.push_back(std::move(input));
  }
  return inputs;
}

static Expected<std::vector<IncrementalSectionState>>
decodeSectionRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                     ArrayRef<uint8_t> strings) {
  Expected<std::vector<SectionRecord>> sectionsOrErr = readTable<SectionRecord>(
      bytes, header.sectionTableOffset, header.sectionCount);
  if (!sectionsOrErr)
    return sectionsOrErr.takeError();

  std::vector<IncrementalSectionState> sections;
  sections.reserve(sectionsOrErr->size());
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
    sections.push_back(std::move(section));
  }
  return sections;
}

static Expected<std::vector<IncrementalChunkSnapshot>>
decodeChunkRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                   ArrayRef<uint8_t> strings) {
  Expected<std::vector<ChunkRecord>> chunksOrErr =
      readTable<ChunkRecord>(bytes, header.chunkTableOffset, header.chunkCount);
  if (!chunksOrErr)
    return chunksOrErr.takeError();

  std::vector<IncrementalChunkSnapshot> chunks;
  chunks.reserve(chunksOrErr->size());
  for (const ChunkRecord &record : *chunksOrErr) {
    Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();
    Expected<IncrementalChunkSnapshot> chunkOrErr =
        decodeChunkSnapshot(record, *keyOrErr);
    if (!chunkOrErr)
      return chunkOrErr.takeError();
    chunks.push_back(std::move(*chunkOrErr));
  }
  return chunks;
}

static Expected<std::vector<IncrementalResolvedSymbolSnapshot>>
decodeSymbolRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                    ArrayRef<uint8_t> strings) {
  Expected<std::vector<SymbolRecord>> symbolsOrErr = readTable<SymbolRecord>(
      bytes, header.symbolTableOffset, header.symbolCount);
  if (!symbolsOrErr)
    return symbolsOrErr.takeError();

  std::vector<IncrementalResolvedSymbolSnapshot> symbols;
  symbols.reserve(symbolsOrErr->size());
  for (const SymbolRecord &record : *symbolsOrErr) {
    Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
    if (!nameOrErr)
      return nameOrErr.takeError();
    Expected<StringRef> auxiliaryKeyOrErr =
        loadString(strings, record.auxiliaryKeyOffset);
    if (!auxiliaryKeyOrErr)
      return auxiliaryKeyOrErr.takeError();
    Expected<IncrementalResolvedSymbolSnapshot> symbolOrErr =
        decodeResolvedSymbol(record, *nameOrErr, *auxiliaryKeyOrErr);
    if (!symbolOrErr)
      return symbolOrErr.takeError();
    symbols.push_back(std::move(*symbolOrErr));
  }
  return symbols;
}

static Expected<std::vector<RawEnvelopeData>>
decodeEnvelopeRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                      ArrayRef<uint8_t> strings) {
  Expected<std::vector<EnvelopeRecord>> envelopesOrErr =
      readTable<EnvelopeRecord>(bytes, header.envelopeTableOffset,
                                header.envelopeCount);
  if (!envelopesOrErr)
    return envelopesOrErr.takeError();

  std::vector<RawEnvelopeData> envelopes;
  envelopes.reserve(envelopesOrErr->size());
  for (const EnvelopeRecord &record : *envelopesOrErr) {
    if (!isValidWireSectionLayoutKind(uint16_t(record.layoutKind)))
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state file has an invalid section layout kind");
    Expected<StringRef> nameOrErr = loadString(strings, record.nameOffset);
    if (!nameOrErr)
      return nameOrErr.takeError();
    RawEnvelopeData envelope;
    envelope.section.name = nameOrErr->str();
    envelope.section.characteristics = record.characteristics;
    envelope.section.rva = record.sectionRVA;
    envelope.layoutKind = static_cast<WireIncrementalSectionLayoutKind>(
        uint16_t(record.layoutKind));
    envelope.maxSectionEndRVA = record.maxSectionEndRVA;
    envelope.activeEndRVA = record.activeEndRVA;
    envelopes.push_back(std::move(envelope));
  }
  return envelopes;
}

static Error decodeSlotRecords(
    ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
    ArrayRef<uint8_t> strings,
    SmallVectorImpl<std::vector<IncrementalPreservedSlot>> &slotsByEnvelope) {
  Expected<std::vector<SlotRecord>> slotsOrErr =
      readTable<SlotRecord>(bytes, header.slotTableOffset, header.slotCount);
  if (!slotsOrErr)
    return slotsOrErr.takeError();

  for (const SlotRecord &record : *slotsOrErr) {
    if (!isValidWireSlotState(uint16_t(record.state)))
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state file has an invalid slot state");
    if (record.envelopeIndex >= slotsByEnvelope.size())
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state slot envelope index is invalid");

    Expected<StringRef> occupantKeyOrErr =
        loadString(strings, record.occupantKeyOffset);
    if (!occupantKeyOrErr)
      return occupantKeyOrErr.takeError();

    IncrementalPreservedSlotState slotState;
    slotState.startRVA = record.startRVA;
    slotState.capacity = record.capacity;
    slotState.committedSize = record.committedSize;
    slotState.minAlignment = record.minAlignment;
    slotState.fillByte = record.fillByte;

    if (static_cast<WireIncrementalSlotState>(uint16_t(record.state)) ==
        WireIncrementalSlotState::Free) {
      slotsByEnvelope[record.envelopeIndex].push_back(
          IncrementalPreservedSlot::make<FreeSlotRecord>(
              FreeSlotRecord{slotState}));
    } else {
      slotsByEnvelope[record.envelopeIndex].push_back(
          IncrementalPreservedSlot::make<OccupiedSlotRecord>(
              OccupiedSlotRecord{slotState, occupantKeyOrErr->str()}));
    }
  }
  return Error::success();
}

static Error
decodePackedSections(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                     ArrayRef<uint8_t> strings,
                     SmallVectorImpl<std::vector<PackedPrefixChunkPlacement>>
                         &packedPlacementsByEnvelope,
                     SmallVectorImpl<uint64_t> &activePrefixSizes,
                     SmallVectorImpl<uint64_t> &reserveSizes) {
  Expected<std::vector<PackedKeyRecord>> packedKeysOrErr =
      readTable<PackedKeyRecord>(bytes, header.packedKeyTableOffset,
                                 header.packedKeyCount);
  if (!packedKeysOrErr)
    return packedKeysOrErr.takeError();

  SmallVector<StringRef, 32> packedKeys;
  packedKeys.reserve(packedKeysOrErr->size());
  for (const PackedKeyRecord &record : *packedKeysOrErr) {
    Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();
    packedKeys.push_back(*keyOrErr);
  }

  Expected<std::vector<PackedSectionRecord>> packedSectionsOrErr =
      readTable<PackedSectionRecord>(bytes, header.packedSectionTableOffset,
                                     header.packedSectionCount);
  if (!packedSectionsOrErr)
    return packedSectionsOrErr.takeError();

  for (const PackedSectionRecord &record : *packedSectionsOrErr) {
    if (record.envelopeIndex >= packedPlacementsByEnvelope.size())
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state packed-section envelope index is invalid");
    if (record.firstRecordKey > packedKeys.size() ||
        packedKeys.size() - record.firstRecordKey < record.recordKeyCount)
      return createStringError(
          inconvertibleErrorCode(),
          "incremental packed section key range is invalid");

    activePrefixSizes[record.envelopeIndex] = record.activePrefixSize;
    reserveSizes[record.envelopeIndex] = record.reserveSize;
    for (StringRef key :
         ArrayRef<StringRef>(packedKeys)
             .slice(record.firstRecordKey, record.recordKeyCount)) {
      packedPlacementsByEnvelope[record.envelopeIndex].push_back(
          PackedPrefixChunkPlacement{key.str(), 0, 0, 1});
    }
  }
  return Error::success();
}

static Error
decodePlacementRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                       ArrayRef<uint8_t> strings,
                       SmallVectorImpl<std::vector<ExistingSlotChunkPlacement>>
                           &slotPlacementsByEnvelope,
                       SmallVectorImpl<std::vector<PackedPrefixChunkPlacement>>
                           &packedPlacementsByEnvelope) {
  Expected<std::vector<PlacementRecord>> placementsOrErr =
      readTable<PlacementRecord>(bytes, header.placementTableOffset,
                                 header.placementCount);
  if (!placementsOrErr)
    return placementsOrErr.takeError();

  for (const PlacementRecord &record : *placementsOrErr) {
    if (!isValidWirePlacementKind(uint16_t(record.kind)))
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state file has an invalid placement kind");
    if (record.envelopeIndex >= slotPlacementsByEnvelope.size() ||
        record.envelopeIndex >= packedPlacementsByEnvelope.size())
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state placement envelope index is invalid");

    Expected<StringRef> keyOrErr = loadString(strings, record.keyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();

    switch (static_cast<WireIncrementalPlacementKind>(uint16_t(record.kind))) {
    case WireIncrementalPlacementKind::ExistingSlot:
      slotPlacementsByEnvelope[record.envelopeIndex].push_back(
          ExistingSlotChunkPlacement{keyOrErr->str(), record.startRVA,
                                     record.size, record.alignment});
      break;
    case WireIncrementalPlacementKind::PackedPrefix: {
      auto &packedPlacements = packedPlacementsByEnvelope[record.envelopeIndex];
      auto it = llvm::find_if(packedPlacements, [&](const auto &placement) {
        return placement.key == *keyOrErr;
      });
      if (it == packedPlacements.end())
        packedPlacements.push_back(PackedPrefixChunkPlacement{
            keyOrErr->str(), record.startRVA, record.size, record.alignment});
      else {
        it->startRVA = record.startRVA;
        it->size = record.size;
        it->alignment = record.alignment;
      }
      break;
    }
    case WireIncrementalPlacementKind::ReusedFreeSlot:
    case WireIncrementalPlacementKind::TailReserve:
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state file contains unsupported persisted placement");
    }
  }
  return Error::success();
}

static Expected<std::vector<IncrementalTextRedirectState>>
decodeRedirectRecords(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header,
                      ArrayRef<uint8_t> strings) {
  Expected<std::vector<RedirectRecord>> redirectsOrErr =
      readTable<RedirectRecord>(bytes, header.redirectTableOffset,
                                header.redirectCount);
  if (!redirectsOrErr)
    return redirectsOrErr.takeError();

  std::vector<IncrementalTextRedirectState> redirects;
  redirects.reserve(redirectsOrErr->size());
  for (const RedirectRecord &record : *redirectsOrErr) {
    Expected<StringRef> keyOrErr = loadString(strings, record.targetKeyOffset);
    if (!keyOrErr)
      return keyOrErr.takeError();
    Expected<StringRef> symbolOrErr =
        loadString(strings, record.canonicalSymbolOffset);
    if (!symbolOrErr)
      return symbolOrErr.takeError();
    redirects.push_back(IncrementalTextRedirectState{
        keyOrErr->str(), symbolOrErr->str(), record.redirectRVA,
        record.redirectCapacity, record.bodyRVA, record.poolThunkRVA});
  }
  return redirects;
}

static Expected<IncrementalTextThunkPoolState>
decodeThunkPool(ArrayRef<uint8_t> bytes, const FileHeaderV5 &header) {
  IncrementalTextThunkPoolState pool;
  Expected<std::vector<PoolStateRecord>> poolOrErr = readTable<PoolStateRecord>(
      bytes, header.poolStateOffset, header.poolStateCount);
  if (!poolOrErr)
    return poolOrErr.takeError();
  if (!poolOrErr->empty()) {
    pool.poolStartRVA = (*poolOrErr)[0].poolStartRVA;
    pool.poolEndRVA = (*poolOrErr)[0].poolEndRVA;
    pool.nextFreeRVA = (*poolOrErr)[0].nextFreeRVA;
  }
  return pool;
}

static void encodeSlotRecord(const IncrementalPreservedSlot &slot,
                             uint32_t envelopeIndex, SlotRecord &record,
                             StringTableBuilder &strings) {
  const IncrementalPreservedSlotState &slotState =
      getIncrementalPreservedSlotState(slot);
  record.occupantKeyOffset = 0;
  matchIncrementalPreservedSlotOccupancy(
      slot,
      [&](const FreeSlotRecord &) {
        record.state = static_cast<uint16_t>(WireIncrementalSlotState::Free);
      },
      [&](const OccupiedSlotRecord &occupied) {
        record.occupantKeyOffset = strings.add(occupied.occupantKey);
        record.state =
            static_cast<uint16_t>(WireIncrementalSlotState::Occupied);
      });
  record.envelopeIndex = envelopeIndex;
  record.minAlignment = slotState.minAlignment;
  record.fillByte = slotState.fillByte;
  record.startRVA = slotState.startRVA;
  record.capacity = slotState.capacity;
  record.committedSize = slotState.committedSize;
}

static void appendSlotRecords(ArrayRef<IncrementalPreservedSlot> slots,
                              uint32_t envelopeIndex,
                              std::vector<SlotRecord> &records,
                              StringTableBuilder &strings) {
  for (const IncrementalPreservedSlot &slot : slots) {
    SlotRecord record = {};
    encodeSlotRecord(slot, envelopeIndex, record, strings);
    records.push_back(record);
  }
}

static Expected<IncrementalBaselineSnapshot>
loadIncrementalStateCurrent(ArrayRef<uint8_t> bytes,
                            const FileHeaderV5 &header) {
  llvm::TimeTraceScope timeScope("Decode incremental state");
  if (static_cast<WireIncrementalLayoutMode>(uint16_t(header.layoutMode)) !=
      WireIncrementalLayoutMode::Slotted)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file is not slotted");

  if (header.stringTableOffset > bytes.size() ||
      header.stringTableSize > bytes.size() - header.stringTableOffset)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state string table is truncated");
  ArrayRef<uint8_t> strings =
      bytes.slice(header.stringTableOffset, header.stringTableSize);

  IncrementalBaselineSnapshot state;
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

  Expected<std::vector<IncrementalInputState>> inputsOrErr =
      decodeInputs(bytes, header, strings);
  if (!inputsOrErr)
    return inputsOrErr.takeError();
  state.inputs = std::move(*inputsOrErr);

  Expected<std::vector<IncrementalSectionState>> sectionStatesOrErr =
      decodeSectionRecords(bytes, header, strings);
  if (!sectionStatesOrErr)
    return sectionStatesOrErr.takeError();

  Expected<std::vector<IncrementalChunkSnapshot>> chunksOrErr =
      decodeChunkRecords(bytes, header, strings);
  if (!chunksOrErr)
    return chunksOrErr.takeError();
  state.chunks = std::move(*chunksOrErr);

  Expected<std::vector<IncrementalResolvedSymbolSnapshot>> symbolsOrErr =
      decodeSymbolRecords(bytes, header, strings);
  if (!symbolsOrErr)
    return symbolsOrErr.takeError();
  state.symbols = std::move(*symbolsOrErr);

  Expected<std::vector<RawEnvelopeData>> envelopesOrErr =
      decodeEnvelopeRecords(bytes, header, strings);
  if (!envelopesOrErr)
    return envelopesOrErr.takeError();
  const std::vector<RawEnvelopeData> &envelopes = *envelopesOrErr;

  SmallVector<std::vector<IncrementalPreservedSlot>, 8> slotsByEnvelope(
      envelopes.size());
  if (Error err = decodeSlotRecords(bytes, header, strings, slotsByEnvelope))
    return std::move(err);

  SmallVector<std::vector<ExistingSlotChunkPlacement>, 8>
      slotPlacementsByEnvelope(envelopes.size());
  SmallVector<std::vector<PackedPrefixChunkPlacement>, 8>
      packedPlacementsByEnvelope(envelopes.size());
  SmallVector<uint64_t, 8> activePrefixSizes(envelopes.size(), 0);
  SmallVector<uint64_t, 8> reserveSizes(envelopes.size(), 0);
  if (Error err = decodePackedSections(bytes, header, strings,
                                       packedPlacementsByEnvelope,
                                       activePrefixSizes, reserveSizes))
    return std::move(err);
  if (Error err = decodePlacementRecords(bytes, header, strings,
                                         slotPlacementsByEnvelope,
                                         packedPlacementsByEnvelope))
    return std::move(err);

  Expected<std::vector<IncrementalTextRedirectState>> redirectsOrErr =
      decodeRedirectRecords(bytes, header, strings);
  if (!redirectsOrErr)
    return redirectsOrErr.takeError();
  Expected<IncrementalTextThunkPoolState> thunkPoolOrErr =
      decodeThunkPool(bytes, header);
  if (!thunkPoolOrErr)
    return thunkPoolOrErr.takeError();

  SmallVector<bool, 8> usedEnvelopes(envelopes.size(), false);
  bool assignedTextRedirectState = false;
  state.sections.reserve(sectionStatesOrErr->size());
  for (const IncrementalSectionState &sectionState : *sectionStatesOrErr) {
    ssize_t matchedEnvelopeIndex = -1;
    for (size_t i = 0; i < envelopes.size(); ++i) {
      if (usedEnvelopes[i])
        continue;
      if (envelopes[i].section.name == sectionState.name &&
          envelopes[i].section.characteristics ==
              sectionState.characteristics) {
        matchedEnvelopeIndex = static_cast<ssize_t>(i);
        break;
      }
    }

    if (matchedEnvelopeIndex < 0) {
      state.sections.push_back(
          IncrementalSectionSnapshot::make<ExactSectionSnapshot>(
              ExactSectionSnapshot{sectionState}));
      continue;
    }

    const RawEnvelopeData &envelope = envelopes[matchedEnvelopeIndex];
    usedEnvelopes[matchedEnvelopeIndex] = true;
    switch (envelope.layoutKind) {
    case WireIncrementalSectionLayoutKind::ExactSectionLayout:
      state.sections.push_back(
          IncrementalSectionSnapshot::make<ExactSectionSnapshot>(
              ExactSectionSnapshot{sectionState}));
      break;
    case WireIncrementalSectionLayoutKind::TextFreeSlots: {
      TextSlotSectionSnapshot text;
      text.slotSection.section = sectionState;
      text.slotSection.maxSectionEndRVA = envelope.maxSectionEndRVA;
      text.slotSection.activeEndRVA = envelope.activeEndRVA;
      text.slotSection.slots = std::move(slotsByEnvelope[matchedEnvelopeIndex]);
      text.slotSection.preservedChunks =
          std::move(slotPlacementsByEnvelope[matchedEnvelopeIndex]);
      text.redirects = std::move(*redirectsOrErr);
      text.thunkPool = *thunkPoolOrErr;
      assignedTextRedirectState = true;
      state.sections.push_back(
          IncrementalSectionSnapshot::make<TextSlotSectionSnapshot>(
              std::move(text)));
      break;
    }
    case WireIncrementalSectionLayoutKind::ReadOnlyDataFreeSlots: {
      ReadOnlySlotSectionSnapshot rdata;
      rdata.slotSection.section = sectionState;
      rdata.slotSection.maxSectionEndRVA = envelope.maxSectionEndRVA;
      rdata.slotSection.activeEndRVA = envelope.activeEndRVA;
      rdata.slotSection.slots =
          std::move(slotsByEnvelope[matchedEnvelopeIndex]);
      rdata.slotSection.preservedChunks =
          std::move(slotPlacementsByEnvelope[matchedEnvelopeIndex]);
      state.sections.push_back(
          IncrementalSectionSnapshot::make<ReadOnlySlotSectionSnapshot>(
              std::move(rdata)));
      break;
    }
    case WireIncrementalSectionLayoutKind::WritableDataFreeSlots: {
      WritableSlotSectionSnapshot data;
      data.slotSection.section = sectionState;
      data.slotSection.maxSectionEndRVA = envelope.maxSectionEndRVA;
      data.slotSection.activeEndRVA = envelope.activeEndRVA;
      data.slotSection.slots = std::move(slotsByEnvelope[matchedEnvelopeIndex]);
      data.slotSection.preservedChunks =
          std::move(slotPlacementsByEnvelope[matchedEnvelopeIndex]);
      state.sections.push_back(
          IncrementalSectionSnapshot::make<WritableSlotSectionSnapshot>(
              std::move(data)));
      break;
    }
    case WireIncrementalSectionLayoutKind::PackedPDataPrefix: {
      PDataPackedPrefixSectionSnapshot pdata;
      pdata.packedSection.section = sectionState;
      pdata.packedSection.activePrefixSize =
          activePrefixSizes[matchedEnvelopeIndex];
      pdata.packedSection.reserveSize = reserveSizes[matchedEnvelopeIndex];
      pdata.packedSection.members =
          std::move(packedPlacementsByEnvelope[matchedEnvelopeIndex]);
      state.sections.push_back(
          IncrementalSectionSnapshot::make<PDataPackedPrefixSectionSnapshot>(
              std::move(pdata)));
      break;
    }
    case WireIncrementalSectionLayoutKind::PackedXDataPrefix: {
      XDataPackedPrefixSectionSnapshot xdata;
      xdata.packedSection.section = sectionState;
      xdata.packedSection.activePrefixSize =
          activePrefixSizes[matchedEnvelopeIndex];
      xdata.packedSection.reserveSize = reserveSizes[matchedEnvelopeIndex];
      xdata.packedSection.members =
          std::move(packedPlacementsByEnvelope[matchedEnvelopeIndex]);
      state.sections.push_back(
          IncrementalSectionSnapshot::make<XDataPackedPrefixSectionSnapshot>(
              std::move(xdata)));
      break;
    }
    }
  }

  if (!redirectsOrErr->empty() && !assignedTextRedirectState)
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state redirect table does not have a text slot section");
  if ((thunkPoolOrErr->poolStartRVA != 0 || thunkPoolOrErr->poolEndRVA != 0 ||
       thunkPoolOrErr->nextFreeRVA != 0) &&
      !assignedTextRedirectState)
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state thunk pool does not have a text slot section");

  for (size_t i = 0; i < envelopes.size(); ++i) {
    if (!usedEnvelopes[i])
      return createStringError(
          inconvertibleErrorCode(),
          "incremental state contains an orphan section envelope");
  }

  return state;
}

} // namespace

Expected<IncrementalBaselineSnapshot> loadIncrementalState(StringRef path) {
  llvm::TimeTraceScope timeScope("Read incremental state file");
  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!buffer)
    return createFileError(path, errorCodeToError(buffer.getError()));

  ArrayRef<uint8_t> bytes(
      reinterpret_cast<const uint8_t *>((*buffer)->getBufferStart()),
      (*buffer)->getBufferSize());
  Expected<FileHeaderV5> headerOrErr = readObject<FileHeaderV5>(bytes, 0);
  if (!headerOrErr)
    return headerOrErr.takeError();

  if (memcmp(headerOrErr->magic, stateMagic, sizeof(stateMagic)) != 0)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an invalid magic");
  if (uint32_t(headerOrErr->version) != currentStateVersion)
    return createStringError(
        inconvertibleErrorCode(),
        "incremental state file has an unsupported version");
  Expected<IncrementalBaselineSnapshot> stateOrErr =
      loadIncrementalStateCurrent(bytes, *headerOrErr);
  if (stateOrErr)
    stateOrErr->stateFileSize = bytes.size();
  return stateOrErr;
}

Error writeIncrementalState(StringRef path,
                            const IncrementalBaselineSnapshot &state) {
  llvm::TimeTraceScope timeScope("Serialize incremental state");
  FileHeaderV5 header = {};
  memcpy(header.magic, stateMagic, sizeof(stateMagic));
  header.version = currentStateVersion;
  header.machine = state.machine;
  header.flags = 0;
  header.layoutMode = static_cast<uint16_t>(WireIncrementalLayoutMode::Slotted);
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
  bool sawTextThunkPool = false;
  for (const IncrementalSectionSnapshot &section : state.sections) {
    SectionRecord sectionRecord = {};
    const IncrementalSectionState &common = getIncrementalSectionState(section);
    sectionRecord.nameOffset = strings.add(common.name);
    sectionRecord.characteristics = common.characteristics;
    sectionRecord.rva = common.rva;
    sectionRecord.fileOffset = common.fileOffset;
    sectionRecord.virtualSize = common.virtualSize;
    sectionRecord.rawSize = common.rawSize;
    sectionRecord.firstChunk = common.firstChunk;
    sectionRecord.chunkCount = common.chunkCount;
    sectionRecords.push_back(sectionRecord);

    section.match(
        [&](const ExactSectionSnapshot &) {},
        [&](const TextSlotSectionSnapshot &text) {
          EnvelopeRecord envelope = {};
          envelope.nameOffset = strings.add(common.name);
          envelope.characteristics = common.characteristics;
          envelope.layoutKind = static_cast<uint16_t>(
              WireIncrementalSectionLayoutKind::TextFreeSlots);
          envelope.sectionRVA = common.rva;
          envelope.maxSectionEndRVA = text.slotSection.maxSectionEndRVA;
          envelope.activeEndRVA = text.slotSection.activeEndRVA;
          uint32_t envelopeIndex = envelopeRecords.size();
          envelopeRecords.push_back(envelope);
          appendSlotRecords(text.slotSection.slots, envelopeIndex, slotRecords,
                            strings);

          for (const ExistingSlotChunkPlacement &placement :
               text.slotSection.preservedChunks) {
            PlacementRecord placementRecord = {};
            placementRecord.keyOffset = strings.add(placement.key);
            placementRecord.envelopeIndex = envelopeIndex;
            placementRecord.alignment = placement.alignment;
            placementRecord.kind = static_cast<uint16_t>(
                WireIncrementalPlacementKind::ExistingSlot);
            placementRecord.startRVA = placement.startRVA;
            placementRecord.size = placement.size;
            placementRecords.push_back(placementRecord);
          }

          for (const IncrementalTextRedirectState &redirect : text.redirects) {
            RedirectRecord redirectRecord = {};
            redirectRecord.targetKeyOffset = strings.add(redirect.targetKey);
            redirectRecord.canonicalSymbolOffset =
                strings.add(redirect.canonicalSymbol);
            redirectRecord.redirectRVA = redirect.redirectRVA;
            redirectRecord.redirectCapacity = redirect.redirectCapacity;
            redirectRecord.bodyRVA = redirect.bodyRVA;
            redirectRecord.poolThunkRVA = redirect.poolThunkRVA;
            redirectRecords.push_back(redirectRecord);
          }

          if (text.thunkPool.poolStartRVA != 0 ||
              text.thunkPool.poolEndRVA != 0 ||
              text.thunkPool.nextFreeRVA != 0) {
            if (sawTextThunkPool)
              report_fatal_error("incremental state snapshot contains multiple "
                                 "text thunk pools");
            sawTextThunkPool = true;
            PoolStateRecord poolRecord = {};
            poolRecord.poolStartRVA = text.thunkPool.poolStartRVA;
            poolRecord.poolEndRVA = text.thunkPool.poolEndRVA;
            poolRecord.nextFreeRVA = text.thunkPool.nextFreeRVA;
            poolStateRecords.push_back(poolRecord);
          }
        },
        [&](const ReadOnlySlotSectionSnapshot &rdata) {
          EnvelopeRecord envelope = {};
          envelope.nameOffset = strings.add(common.name);
          envelope.characteristics = common.characteristics;
          envelope.layoutKind = static_cast<uint16_t>(
              WireIncrementalSectionLayoutKind::ReadOnlyDataFreeSlots);
          envelope.sectionRVA = common.rva;
          envelope.maxSectionEndRVA = rdata.slotSection.maxSectionEndRVA;
          envelope.activeEndRVA = rdata.slotSection.activeEndRVA;
          uint32_t envelopeIndex = envelopeRecords.size();
          envelopeRecords.push_back(envelope);
          appendSlotRecords(rdata.slotSection.slots, envelopeIndex, slotRecords,
                            strings);

          for (const ExistingSlotChunkPlacement &placement :
               rdata.slotSection.preservedChunks) {
            PlacementRecord placementRecord = {};
            placementRecord.keyOffset = strings.add(placement.key);
            placementRecord.envelopeIndex = envelopeIndex;
            placementRecord.alignment = placement.alignment;
            placementRecord.kind = static_cast<uint16_t>(
                WireIncrementalPlacementKind::ExistingSlot);
            placementRecord.startRVA = placement.startRVA;
            placementRecord.size = placement.size;
            placementRecords.push_back(placementRecord);
          }
        },
        [&](const WritableSlotSectionSnapshot &data) {
          EnvelopeRecord envelope = {};
          envelope.nameOffset = strings.add(common.name);
          envelope.characteristics = common.characteristics;
          envelope.layoutKind = static_cast<uint16_t>(
              WireIncrementalSectionLayoutKind::WritableDataFreeSlots);
          envelope.sectionRVA = common.rva;
          envelope.maxSectionEndRVA = data.slotSection.maxSectionEndRVA;
          envelope.activeEndRVA = data.slotSection.activeEndRVA;
          uint32_t envelopeIndex = envelopeRecords.size();
          envelopeRecords.push_back(envelope);
          appendSlotRecords(data.slotSection.slots, envelopeIndex, slotRecords,
                            strings);

          for (const ExistingSlotChunkPlacement &placement :
               data.slotSection.preservedChunks) {
            PlacementRecord placementRecord = {};
            placementRecord.keyOffset = strings.add(placement.key);
            placementRecord.envelopeIndex = envelopeIndex;
            placementRecord.alignment = placement.alignment;
            placementRecord.kind = static_cast<uint16_t>(
                WireIncrementalPlacementKind::ExistingSlot);
            placementRecord.startRVA = placement.startRVA;
            placementRecord.size = placement.size;
            placementRecords.push_back(placementRecord);
          }
        },
        [&](const PDataPackedPrefixSectionSnapshot &pdata) {
          EnvelopeRecord envelope = {};
          envelope.nameOffset = strings.add(common.name);
          envelope.characteristics = common.characteristics;
          envelope.layoutKind = static_cast<uint16_t>(
              WireIncrementalSectionLayoutKind::PackedPDataPrefix);
          envelope.sectionRVA = common.rva;
          envelope.maxSectionEndRVA = common.rva +
                                      pdata.packedSection.activePrefixSize +
                                      pdata.packedSection.reserveSize;
          envelope.activeEndRVA =
              common.rva + pdata.packedSection.activePrefixSize;
          uint32_t envelopeIndex = envelopeRecords.size();
          envelopeRecords.push_back(envelope);

          PackedSectionRecord packedRecord = {};
          packedRecord.envelopeIndex = envelopeIndex;
          packedRecord.firstRecordKey = packedKeyRecords.size();
          packedRecord.recordKeyCount = pdata.packedSection.members.size();
          packedRecord.activePrefixSize = pdata.packedSection.activePrefixSize;
          packedRecord.reserveSize = pdata.packedSection.reserveSize;
          for (const PackedPrefixChunkPlacement &placement :
               pdata.packedSection.members) {
            PackedKeyRecord keyRecord = {};
            keyRecord.keyOffset = strings.add(placement.key);
            packedKeyRecords.push_back(keyRecord);

            PlacementRecord placementRecord = {};
            placementRecord.keyOffset = strings.add(placement.key);
            placementRecord.envelopeIndex = envelopeIndex;
            placementRecord.alignment = placement.alignment;
            placementRecord.kind = static_cast<uint16_t>(
                WireIncrementalPlacementKind::PackedPrefix);
            placementRecord.startRVA = placement.startRVA;
            placementRecord.size = placement.size;
            placementRecords.push_back(placementRecord);
          }
          packedSectionRecords.push_back(packedRecord);
        },
        [&](const XDataPackedPrefixSectionSnapshot &xdata) {
          EnvelopeRecord envelope = {};
          envelope.nameOffset = strings.add(common.name);
          envelope.characteristics = common.characteristics;
          envelope.layoutKind = static_cast<uint16_t>(
              WireIncrementalSectionLayoutKind::PackedXDataPrefix);
          envelope.sectionRVA = common.rva;
          envelope.maxSectionEndRVA = common.rva +
                                      xdata.packedSection.activePrefixSize +
                                      xdata.packedSection.reserveSize;
          envelope.activeEndRVA =
              common.rva + xdata.packedSection.activePrefixSize;
          uint32_t envelopeIndex = envelopeRecords.size();
          envelopeRecords.push_back(envelope);

          PackedSectionRecord packedRecord = {};
          packedRecord.envelopeIndex = envelopeIndex;
          packedRecord.firstRecordKey = packedKeyRecords.size();
          packedRecord.recordKeyCount = xdata.packedSection.members.size();
          packedRecord.activePrefixSize = xdata.packedSection.activePrefixSize;
          packedRecord.reserveSize = xdata.packedSection.reserveSize;
          for (const PackedPrefixChunkPlacement &placement :
               xdata.packedSection.members) {
            PackedKeyRecord keyRecord = {};
            keyRecord.keyOffset = strings.add(placement.key);
            packedKeyRecords.push_back(keyRecord);

            PlacementRecord placementRecord = {};
            placementRecord.keyOffset = strings.add(placement.key);
            placementRecord.envelopeIndex = envelopeIndex;
            placementRecord.alignment = placement.alignment;
            placementRecord.kind = static_cast<uint16_t>(
                WireIncrementalPlacementKind::PackedPrefix);
            placementRecord.startRVA = placement.startRVA;
            placementRecord.size = placement.size;
            placementRecords.push_back(placementRecord);
          }
          packedSectionRecords.push_back(packedRecord);
        });
  }

  chunkRecords.reserve(state.chunks.size());
  for (const IncrementalChunkSnapshot &chunk : state.chunks) {
    ChunkRecord record = {};
    encodeChunkSnapshot(chunk, record, strings);
    chunkRecords.push_back(record);
  }

  symbolRecords.reserve(state.symbols.size());
  for (const IncrementalResolvedSymbolSnapshot &symbol : state.symbols) {
    SymbolRecord record = {};
    encodeResolvedSymbol(symbol, record, strings);
    symbolRecords.push_back(record);
  }

  header.inputTableOffset = sizeof(FileHeaderV5);
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
  header.slotTableOffset = header.envelopeTableOffset +
                           envelopeRecords.size() * sizeof(EnvelopeRecord);
  header.slotCount = slotRecords.size();
  header.packedSectionTableOffset =
      header.slotTableOffset + slotRecords.size() * sizeof(SlotRecord);
  header.packedSectionCount = packedSectionRecords.size();
  header.packedKeyTableOffset =
      header.packedSectionTableOffset +
      packedSectionRecords.size() * sizeof(PackedSectionRecord);
  header.packedKeyCount = packedKeyRecords.size();
  header.placementTableOffset =
      header.packedKeyTableOffset +
      packedKeyRecords.size() * sizeof(PackedKeyRecord);
  header.placementCount = placementRecords.size();
  header.redirectTableOffset =
      header.placementTableOffset +
      placementRecords.size() * sizeof(PlacementRecord);
  header.redirectCount = redirectRecords.size();
  header.poolStateOffset = header.redirectTableOffset +
                           redirectRecords.size() * sizeof(RedirectRecord);
  header.poolStateCount = poolStateRecords.size();
  header.stringTableOffset = header.poolStateOffset +
                             poolStateRecords.size() * sizeof(PoolStateRecord);
  header.stringTableSize = strings.data().size();

  std::vector<char> bytes;
  bytes.reserve(header.stringTableOffset + header.stringTableSize);
  appendObject(bytes, header);
  for (const InputRecord &record : inputRecords)
    appendObject(bytes, record);
  for (const SectionRecord &record : sectionRecords)
    appendObject(bytes, record);
  for (const ChunkRecord &record : chunkRecords)
    appendObject(bytes, record);
  for (const SymbolRecord &record : symbolRecords)
    appendObject(bytes, record);
  for (const EnvelopeRecord &record : envelopeRecords)
    appendObject(bytes, record);
  for (const SlotRecord &record : slotRecords)
    appendObject(bytes, record);
  for (const PackedSectionRecord &record : packedSectionRecords)
    appendObject(bytes, record);
  for (const PackedKeyRecord &record : packedKeyRecords)
    appendObject(bytes, record);
  for (const PlacementRecord &record : placementRecords)
    appendObject(bytes, record);
  for (const RedirectRecord &record : redirectRecords)
    appendObject(bytes, record);
  for (const PoolStateRecord &record : poolStateRecords)
    appendObject(bytes, record);
  bytes.insert(bytes.end(), strings.data().begin(), strings.data().end());

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

} // namespace lld::coff
