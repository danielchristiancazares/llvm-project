#ifndef LLD_COFF_INCREMENTAL_H
#define LLD_COFF_INCREMENTAL_H

#include "IncrementalState.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class MemoryBuffer;
}

namespace lld::coff {

class Chunk;
class COFFLinkerContext;
class ObjFile;
class SectionChunk;

class IncrementalLinkSession {
public:
  bool canWriteState = false;
  bool stateLoaded = false;
  bool softConfigChanged = false;
  IncrementalStateFile state;
  std::unique_ptr<llvm::MemoryBuffer> oldImage;
  std::vector<uint64_t> currentInputHashes;
  std::vector<std::string> currentInputNames;
  std::vector<std::string> currentParentNames;
  llvm::DenseMap<const ObjFile *, uint32_t> inputIndices;
  llvm::DenseSet<const ObjFile *> changedInputs;
  llvm::DenseMap<const SectionChunk *, llvm::ArrayRef<uint8_t>> reusedChunkData;
};

void prepareIncrementalLink(COFFLinkerContext &ctx);
void finalizeIncrementalLink(COFFLinkerContext &ctx);

void setIncrementalFallback(COFFLinkerContext &ctx,
                            IncrementalFallbackReason reason,
                            const llvm::Twine &detail = {});
llvm::StringRef incrementalFallbackReasonToString(
    IncrementalFallbackReason reason);

uint64_t computeIncrementalHardConfigHash(const Configuration &config);
uint64_t computeIncrementalSoftConfigHash(const Configuration &config);

bool prepareCurrentIncrementalInputs(COFFLinkerContext &ctx,
                                     IncrementalLinkSession &session);
IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk);
std::string getIncrementalChunkKey(const IncrementalLinkSession &session,
                                   const Chunk &chunk);
uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk);

} // namespace lld::coff

#endif
