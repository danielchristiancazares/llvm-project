#ifndef LLD_COFF_INCREMENTALSTATE_H
#define LLD_COFF_INCREMENTALSTATE_H

#include "Config.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lld::coff {

enum class IncrementalChunkKind : uint16_t {
  ObjSection = 1,
  Synthetic = 2,
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

struct IncrementalStateFile {
  uint32_t version = 2;
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
};

llvm::Expected<IncrementalStateFile> loadIncrementalState(llvm::StringRef path);
llvm::Error writeIncrementalState(llvm::StringRef path,
                                  const IncrementalStateFile &state);

} // namespace lld::coff

#endif
