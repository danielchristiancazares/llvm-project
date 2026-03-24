#ifndef LLD_COFF_INCREMENTAL_H
#define LLD_COFF_INCREMENTAL_H

#include "IncrementalState.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"
#include <memory>
#include <optional>
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
  std::vector<uint64_t> currentArchiveOffsets;
  llvm::DenseMap<const ObjFile *, uint32_t> inputIndices;
  llvm::DenseSet<const ObjFile *> changedInputs;
  llvm::DenseMap<const SectionChunk *, llvm::ArrayRef<uint8_t>> reusedChunkData;
  llvm::StringMap<uint32_t> oldPlacementIndices;
  llvm::StringMap<uint32_t> oldEnvelopeIndices;
  llvm::DenseMap<const Chunk *, IncrementalPlacementKind> placementKinds;
  llvm::DenseSet<const Chunk *> rewrittenChunks;
  llvm::StringSet<> replayableArchives;
  llvm::StringSet<> expectedArchiveMembers;
  llvm::StringSet<> loadedArchiveMembers;
};

void prepareIncrementalLink(COFFLinkerContext &ctx);
void finalizeIncrementalLinkPlan(COFFLinkerContext &ctx);
void finalizeIncrementalLink(COFFLinkerContext &ctx);
void noteIncrementalArchiveMemberLoad(COFFLinkerContext &ctx,
                                      llvm::StringRef archiveName,
                                      uint64_t archiveOffset,
                                      llvm::StringRef memberName);

void setIncrementalFallback(COFFLinkerContext &ctx,
                            IncrementalFallbackReason reason,
                            const llvm::Twine &detail = {});
llvm::StringRef incrementalFallbackReasonToString(
    IncrementalFallbackReason reason);

uint64_t computeIncrementalHardConfigHash(const Configuration &config);
uint64_t computeIncrementalSoftConfigHash(const Configuration &config);

bool prepareCurrentIncrementalInputs(COFFLinkerContext &ctx,
                                     IncrementalLinkSession &session);
IncrementalSlotClass classifyIncrementalSection(llvm::StringRef name,
                                                uint32_t characteristics);
bool isIncrementalSlotReuseClass(IncrementalSlotClass slotClass);
bool isIncrementalPackedClass(IncrementalSlotClass slotClass);
uint8_t getIncrementalFillByte(IncrementalSlotClass slotClass);
std::optional<size_t> findBestFitIncrementalFreeSlot(
    llvm::ArrayRef<IncrementalSlotRecordState> slots, uint64_t size,
    uint32_t alignment);
std::optional<uint64_t> allocateIncrementalTailReserve(uint64_t tailCursor,
                                                       uint64_t maxSectionEndRVA,
                                                       uint64_t size,
                                                       uint32_t alignment);
bool isIncrementalAmd64Rel32InRange(uint16_t type, uint64_t sourceRVA,
                                    uint64_t targetRVA);
IncrementalChunkKind classifyIncrementalChunk(const Chunk &chunk);
std::string getIncrementalChunkKey(const IncrementalLinkSession &session,
                                   const Chunk &chunk);
uint64_t computeIncrementalSymbolHash(const SectionChunk &chunk);

} // namespace lld::coff

#endif
