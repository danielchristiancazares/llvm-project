#include "IncrementalRedirects.h"
#include "COFFLinkerContext.h"
#include "Symbols.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Object/COFF.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::object;

namespace lld::coff {

bool isIncrementalControlFlowRefKind(IncrementalRefKind kind) {
  switch (kind) {
  case IncrementalRefKind::DirectCall:
  case IncrementalRefKind::DirectJump:
  case IncrementalRefKind::DirectCondJump:
    return true;
  case IncrementalRefKind::Unknown:
  case IncrementalRefKind::DataAddress:
  case IncrementalRefKind::RipRelativeData:
  case IncrementalRefKind::NonEntryCodeRef:
    return false;
  }
  llvm_unreachable("unknown incremental ref kind");
}

std::optional<IncrementalCanonicalEntry>
findIncrementalCanonicalEntry(const SectionChunk &chunk) {
  if (chunk.sym && chunk.sym->getValue() == 0)
    return IncrementalCanonicalEntry{chunk.sym->getName().str(), 0};

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
  if (candidates.empty())
    return std::nullopt;
  llvm::sort(candidates);
  return IncrementalCanonicalEntry{candidates.front(), 0};
}

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

IncrementalRefKind classifyIncrementalAmd64Rel32Ref(
    const SectionChunk &source, const coff_relocation &rel, uint32_t targetOffset,
    bool &redirectEligible) {
  redirectEligible = false;
  ArrayRef<uint8_t> contents = source.getContents();
  if (rel.VirtualAddress > contents.size())
    return IncrementalRefKind::Unknown;

  if (targetOffset != 0)
    return IncrementalRefKind::NonEntryCodeRef;

  if (rel.VirtualAddress >= 1) {
    uint8_t opcode = contents[rel.VirtualAddress - 1];
    if (opcode == 0xE8) {
      redirectEligible = true;
      return IncrementalRefKind::DirectCall;
    }
    if (opcode == 0xE9) {
      redirectEligible = true;
      return IncrementalRefKind::DirectJump;
    }
  }

  if (rel.VirtualAddress >= 2 && contents[rel.VirtualAddress - 2] == 0x0F) {
    uint8_t opcode = contents[rel.VirtualAddress - 1];
    if ((opcode & 0xF0) == 0x80) {
      redirectEligible = true;
      return IncrementalRefKind::DirectCondJump;
    }
  }

  return IncrementalRefKind::Unknown;
}

std::vector<IncrementalEdgeState>
buildIncrementalEdgeStates(COFFLinkerContext &ctx,
                           IncrementalLinkSession &session) {
  std::vector<IncrementalEdgeState> edges;
  StringMap<bool> targetHasCanonicalEntry;

  for (OutputSection *section : ctx.outputSections) {
    for (Chunk *chunk : section->chunks) {
      auto *source = dyn_cast<SectionChunk>(chunk);
      if (!source || !source->file)
        continue;

      std::string sourceKey = getIncrementalChunkKey(session, *source);
      for (const coff_relocation &rel : source->getRelocs()) {
        auto *sym =
            dyn_cast_or_null<Defined>(source->file->getSymbol(rel.SymbolTableIndex));
        if (!sym)
          continue;

        auto *targetChunk = dyn_cast<SectionChunk>(sym->getChunk());
        if (!targetChunk)
          continue;
        if (!(targetChunk->getOutputCharacteristics() & llvm::COFF::IMAGE_SCN_CNT_CODE))
          continue;

        std::string targetKey = getIncrementalChunkKey(session, *targetChunk);
        uint32_t targetOffset = sym->getRVA() - targetChunk->getRVA();

        bool redirectEligible = false;
        IncrementalRefKind kind = IncrementalRefKind::Unknown;
        if (source->getMachine() == AMD64) {
          auto it = targetHasCanonicalEntry.find(targetKey);
          if (it == targetHasCanonicalEntry.end()) {
            bool hasEntry = findIncrementalCanonicalEntry(*targetChunk).has_value();
            it = targetHasCanonicalEntry.try_emplace(targetKey, hasEntry).first;
          }
          kind = classifyIncrementalAmd64Rel32Ref(*source, rel, targetOffset,
                                                  redirectEligible);
          redirectEligible &= it->second;
        }

        IncrementalEdgeState edge;
        edge.sourceKey = sourceKey;
        edge.targetKey = targetKey;
        edge.kind = kind;
        edge.sourceOffset = rel.VirtualAddress;
        edge.targetOffset = targetOffset;
        edge.redirectEligible = redirectEligible;
        edges.push_back(std::move(edge));
      }
    }
  }

  llvm::sort(edges, [](const IncrementalEdgeState &lhs,
                       const IncrementalEdgeState &rhs) {
    return std::tie(lhs.sourceKey, lhs.sourceOffset, lhs.targetKey, lhs.targetOffset,
                    lhs.kind) <
           std::tie(rhs.sourceKey, rhs.sourceOffset, rhs.targetKey, rhs.targetOffset,
                    rhs.kind);
  });
  return edges;
}

} // namespace lld::coff
