#include "IncrementalLayout.h"
#include "COFFLinkerContext.h"
#include "Incremental.h"
#include "InputFiles.h"

using namespace llvm;

namespace lld::coff {

static void clearReuseState(COFFLinkerContext &ctx) {
  if (!ctx.incrementalSession)
    return;
  ctx.incrementalSession->reusedChunkData.clear();
}

bool applyIncrementalLayout(COFFLinkerContext &ctx,
                            IncrementalLayoutResult &result) {
  if (!ctx.incrementalSession || !ctx.incrementalSession->stateLoaded)
    return false;

  IncrementalLinkSession &session = *ctx.incrementalSession;
  clearReuseState(ctx);

  SmallVector<OutputSection *, 16> activeSections;
  for (OutputSection *section : ctx.outputSections)
    if (section->getVirtualSize() != 0)
      activeSections.push_back(section);

  if (activeSections.size() != session.state.sections.size()) {
    setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                           "output section count changed");
    return false;
  }

  StringRef oldImage = session.oldImage->getBuffer();
  ArrayRef<uint8_t> oldBytes(
      reinterpret_cast<const uint8_t *>(oldImage.data()), oldImage.size());

  for (size_t sectionIndex = 0; sectionIndex < activeSections.size();
       ++sectionIndex) {
    OutputSection *currentSection = activeSections[sectionIndex];
    const IncrementalSectionState &oldSection = session.state.sections[sectionIndex];

    if (currentSection->name != oldSection.name ||
        currentSection->header.Characteristics != oldSection.characteristics ||
        currentSection->chunks.size() != oldSection.chunkCount) {
      clearReuseState(ctx);
      setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                             "section layout changed: " + currentSection->name);
      return false;
    }

    if (oldSection.firstChunk > session.state.chunks.size() ||
        session.state.chunks.size() - oldSection.firstChunk <
            oldSection.chunkCount) {
      clearReuseState(ctx);
      setIncrementalFallback(ctx, IncrementalFallbackReason::InvalidState,
                             "chunk table range is invalid");
      return false;
    }

    currentSection->header.VirtualAddress = oldSection.rva;
    currentSection->header.PointerToRawData = oldSection.fileOffset;
    currentSection->header.VirtualSize = oldSection.virtualSize;
    currentSection->header.SizeOfRawData = oldSection.rawSize;

    for (size_t chunkOffset = 0; chunkOffset < currentSection->chunks.size();
         ++chunkOffset) {
      Chunk *currentChunk = currentSection->chunks[chunkOffset];
      const IncrementalChunkState &oldChunk =
          session.state.chunks[oldSection.firstChunk + chunkOffset];

      if (oldChunk.sectionIndex != sectionIndex ||
          classifyIncrementalChunk(*currentChunk) != oldChunk.kind ||
          getIncrementalChunkKey(session, *currentChunk) != oldChunk.key) {
        clearReuseState(ctx);
        setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                               "chunk key mismatch in section " +
                                   currentSection->name);
        return false;
      }

      if (currentChunk->getSize() > oldChunk.slotCapacity) {
        clearReuseState(ctx);
        setIncrementalFallback(ctx, IncrementalFallbackReason::SlotOverflow,
                               "chunk grew past prior slot");
        return false;
      }

      currentChunk->setRVA(oldChunk.rva);

      auto *sectionChunk = dyn_cast<SectionChunk>(currentChunk);
      if (!sectionChunk || oldChunk.kind != IncrementalChunkKind::ObjSection)
        continue;

      auto fileIt = session.inputIndices.find(sectionChunk->file);
      if (fileIt == session.inputIndices.end() || fileIt->second != oldChunk.inputIndex ||
          oldChunk.inputIndex >= session.state.inputs.size()) {
        clearReuseState(ctx);
        setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                               "input index mismatch for section chunk");
        return false;
      }

      if (session.changedInputs.contains(sectionChunk->file)) {
        if (computeIncrementalSymbolHash(*sectionChunk) != oldChunk.symbolHash) {
          clearReuseState(ctx);
          setIncrementalFallback(ctx, IncrementalFallbackReason::LayoutChanged,
                                 "symbol layout changed inside section chunk");
          return false;
        }
        continue;
      }

      uint64_t fileOffset = oldSection.fileOffset + (oldChunk.rva - oldSection.rva);
      if (fileOffset > oldBytes.size() ||
          oldBytes.size() - fileOffset < sectionChunk->getSize()) {
        clearReuseState(ctx);
        setIncrementalFallback(ctx, IncrementalFallbackReason::OutputMismatch,
                               "reused chunk bytes extend past prior image");
        return false;
      }
      session.reusedChunkData.try_emplace(
          sectionChunk, oldBytes.slice(fileOffset, sectionChunk->getSize()));
    }
  }

  result.fileSize = session.state.outputSize;
  result.sizeOfImage = session.state.sizeOfImage;
  result.sizeOfHeaders = session.state.sizeOfHeaders;
  ctx.config.incrementalLinkActive = true;
  ctx.config.incrementalFallbackReason = IncrementalFallbackReason::None;
  ctx.config.incrementalFallbackDetail.clear();
  if (ctx.config.verbose)
    Log(ctx) << "incremental: exact-layout reuse active";
  return true;
}

} // namespace lld::coff
