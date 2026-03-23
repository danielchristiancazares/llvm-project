#include "IncrementalState.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstring>

using namespace llvm;
using namespace llvm::support;
using namespace llvm::support::endian;

namespace lld::coff {

namespace {

constexpr char stateMagic[8] = {'L', 'L', 'I', 'L', 'K', '6', '4', '\0'};
constexpr uint32_t stateVersion = 1;

struct FileHeader {
  char magic[8];
  ulittle32_t version;
  ulittle16_t machine;
  ulittle16_t flags;
  ulittle64_t outputHash;
  ulittle64_t outputSize;
  ulittle64_t hardConfigHash;
  ulittle64_t softConfigHash;
  ulittle64_t sizeOfHeaders;
  ulittle64_t sizeOfImage;
  ulittle64_t outputPathOffset;
  ulittle64_t inputTableOffset;
  ulittle64_t inputCount;
  ulittle64_t sectionTableOffset;
  ulittle64_t sectionCount;
  ulittle64_t chunkTableOffset;
  ulittle64_t chunkCount;
  ulittle64_t stringTableOffset;
  ulittle64_t stringTableSize;
};

struct InputRecord {
  ulittle64_t nameOffset;
  ulittle64_t parentOffset;
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

} // namespace

Expected<IncrementalStateFile> loadIncrementalState(StringRef path) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> buffer = MemoryBuffer::getFile(
      path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  if (!buffer)
    return createFileError(path, errorCodeToError(buffer.getError()));

  ArrayRef<uint8_t> bytes(reinterpret_cast<const uint8_t *>(
                              (*buffer)->getBufferStart()),
                          (*buffer)->getBufferSize());
  Expected<FileHeader> headerOrErr = readObject<FileHeader>(bytes, 0);
  if (!headerOrErr)
    return headerOrErr.takeError();
  FileHeader header = *headerOrErr;

  if (memcmp(header.magic, stateMagic, sizeof(stateMagic)) != 0)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an invalid magic");
  if (header.version != stateVersion)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state file has an unsupported version");

  if (header.stringTableOffset > bytes.size() ||
      header.stringTableSize > bytes.size() - header.stringTableOffset)
    return createStringError(inconvertibleErrorCode(),
                             "incremental state string table is truncated");
  ArrayRef<uint8_t> strings =
      bytes.slice(header.stringTableOffset, header.stringTableSize);

  IncrementalStateFile state;
  state.version = header.version;
  state.machine = static_cast<llvm::COFF::MachineTypes>(uint16_t(header.machine));
  state.outputHash = header.outputHash;
  state.outputSize = header.outputSize;
  state.hardConfigHash = header.hardConfigHash;
  state.softConfigHash = header.softConfigHash;
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

  return state;
}

Error writeIncrementalState(StringRef path, const IncrementalStateFile &state) {
  FileHeader header = {};
  memcpy(header.magic, stateMagic, sizeof(stateMagic));
  header.version = stateVersion;
  header.machine = state.machine;
  header.flags = 0;
  header.outputHash = state.outputHash;
  header.outputSize = state.outputSize;
  header.hardConfigHash = state.hardConfigHash;
  header.softConfigHash = state.softConfigHash;
  header.sizeOfHeaders = state.sizeOfHeaders;
  header.sizeOfImage = state.sizeOfImage;

  StringTableBuilder strings;
  std::vector<InputRecord> inputRecords;
  std::vector<SectionRecord> sectionRecords;
  std::vector<ChunkRecord> chunkRecords;

  header.outputPathOffset = strings.add(state.outputPath);

  inputRecords.reserve(state.inputs.size());
  for (const IncrementalInputState &input : state.inputs) {
    InputRecord record = {};
    record.nameOffset = strings.add(input.name);
    record.parentOffset = strings.add(input.parentName);
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

  header.inputTableOffset = sizeof(FileHeader);
  header.inputCount = inputRecords.size();
  header.sectionTableOffset =
      header.inputTableOffset + inputRecords.size() * sizeof(InputRecord);
  header.sectionCount = sectionRecords.size();
  header.chunkTableOffset =
      header.sectionTableOffset + sectionRecords.size() * sizeof(SectionRecord);
  header.chunkCount = chunkRecords.size();
  header.stringTableOffset =
      header.chunkTableOffset + chunkRecords.size() * sizeof(ChunkRecord);
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
