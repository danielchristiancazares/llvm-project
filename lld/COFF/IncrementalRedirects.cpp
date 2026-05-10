#include "IncrementalRedirects.h"
#include "COFFLinkerContext.h"
#include "Symbols.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/COFF.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::object;

namespace lld::coff {

namespace {

static bool hasIncrementalCanonicalEntry(const SectionChunk &chunk) {
  if (chunk.sym && chunk.sym->getValue() == 0)
    return true;

  SmallVector<std::string, 4> candidates;
  uint32_t sectionNumber = chunk.getSectionNumber();
  for (const SymbolRef &ref : chunk.file->getCOFFObj()->symbols()) {
    COFFSymbolRef symbol = chunk.file->getCOFFObj()->getCOFFSymbol(ref);
    if (!symbol.isExternal() || symbol.getValue() != 0 ||
        symbol.getSectionNumber() != static_cast<int32_t>(sectionNumber))
      continue;
    Expected<StringRef> nameOrErr = chunk.file->getCOFFObj()->getSymbolName(symbol);
    if (!nameOrErr)
      continue;
    candidates.push_back(nameOrErr->str());
  }
  return !candidates.empty();
}

static IncrementalEdgeRouting classifyIncrementalAmd64Rel32Routing(
    const SectionChunk &source, const coff_relocation &rel,
    uint32_t targetOffset, bool targetHasCanonicalEntry) {
  if (rel.VirtualAddress > source.getContents().size())
    return IncrementalEdgeRouting::BodyOnlyReference;

  if (targetOffset != 0 || !targetHasCanonicalEntry)
    return IncrementalEdgeRouting::BodyOnlyReference;

  ArrayRef<uint8_t> contents = source.getContents();
  if (rel.VirtualAddress >= 1) {
    uint8_t opcode = contents[rel.VirtualAddress - 1];
    if (opcode == 0xE8 || opcode == 0xE9)
      return IncrementalEdgeRouting::RedirectEligibleEntryReference;
  }

  if (rel.VirtualAddress >= 2 && contents[rel.VirtualAddress - 2] == 0x0F) {
    uint8_t opcode = contents[rel.VirtualAddress - 1];
    if ((opcode & 0xF0) == 0x80)
      return IncrementalEdgeRouting::RedirectEligibleEntryReference;
  }

  return IncrementalEdgeRouting::BodyOnlyReference;
}

} // namespace

Defined *findIncrementalCanonicalEntrySymbol(const SectionChunk &chunk) {
  if (chunk.sym && chunk.sym->getValue() == 0)
    return chunk.sym;

  SmallVector<Defined *, 4> candidates;
  for (Symbol *symbol : chunk.file->getSymbols()) {
    auto *def = dyn_cast_or_null<Defined>(symbol);
    if (!def || def->getChunk() != &chunk || def->getRVA() != chunk.getRVA())
      continue;
    if (auto *coff = dyn_cast<DefinedCOFF>(def))
      if (!coff->getCOFFSymbol().isExternal())
        continue;
    candidates.push_back(def);
  }
  if (candidates.empty())
    return nullptr;
  llvm::sort(candidates, [](Defined *lhs, Defined *rhs) {
    return lhs->getName() < rhs->getName();
  });
  return candidates.front();
}

std::vector<IncrementalEdgeState>
buildIncrementalEdgeStates(COFFLinkerContext &ctx,
                           const IncrementalInputIndexMap &inputIndices) {
  std::vector<IncrementalEdgeState> edges;
  StringMap<bool> targetHasCanonicalEntry;

  for (OutputSection *section : ctx.outputSections) {
    for (Chunk *chunk : section->chunks) {
      auto *source = dyn_cast<SectionChunk>(chunk);
      if (!source || !source->file)
        continue;

      std::string sourceKey = getIncrementalChunkKey(inputIndices, *source);
      for (const coff_relocation &rel : source->getRelocs()) {
        auto *sym =
            dyn_cast_or_null<Defined>(source->file->getSymbol(rel.SymbolTableIndex));
        if (!sym)
          continue;

        // Incremental edge tracking only needs stable object-backed section
        // chunks. Import, local-import, and writer-synthetic targets are not
        // reused via preserved chunk placement and may not be materialized yet.
        auto *target = dyn_cast<DefinedRegular>(sym);
        if (!target)
          continue;

        auto *targetChunk = dyn_cast<SectionChunk>(target->getChunk());
        if (!targetChunk)
          continue;

        std::string targetKey = getIncrementalChunkKey(inputIndices, *targetChunk);
        uint32_t targetOffset = sym->getRVA() - targetChunk->getRVA();

        IncrementalEdgeRouting routing =
            IncrementalEdgeRouting::BodyOnlyReference;
        bool targetIsCode =
            targetChunk->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_CNT_CODE;
        // Byte reuse must invalidate stale relocations for any moved section
        // chunk target. Redirect rewriting stays limited to code-entry references.
        if (targetIsCode && source->getMachine() == AMD64) {
          auto it = targetHasCanonicalEntry.find(targetKey);
          if (it == targetHasCanonicalEntry.end()) {
            bool hasEntry = hasIncrementalCanonicalEntry(*targetChunk);
            it = targetHasCanonicalEntry.try_emplace(targetKey, hasEntry).first;
          }
          routing = classifyIncrementalAmd64Rel32Routing(*source, rel,
                                                         targetOffset,
                                                         it->second);
        }

        IncrementalEdgeState edge;
        edge.sourceKey = sourceKey;
        edge.targetKey = targetKey;
        edge.routing = routing;
        edge.sourceOffset = rel.VirtualAddress;
        edge.targetOffset = targetOffset;
        edges.push_back(std::move(edge));
      }
    }
  }

  llvm::sort(edges, [](const IncrementalEdgeState &lhs,
                       const IncrementalEdgeState &rhs) {
    return std::tie(lhs.sourceKey, lhs.sourceOffset, lhs.targetKey, lhs.targetOffset,
                    lhs.routing) <
           std::tie(rhs.sourceKey, rhs.sourceOffset, rhs.targetKey, rhs.targetOffset,
                    rhs.routing);
  });
  return edges;
}

} // namespace lld::coff
