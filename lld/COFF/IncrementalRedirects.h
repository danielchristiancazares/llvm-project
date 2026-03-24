#ifndef LLD_COFF_INCREMENTALREDIRECTS_H
#define LLD_COFF_INCREMENTALREDIRECTS_H

#include "Incremental.h"
#include "llvm/Object/COFF.h"
#include <optional>
#include <vector>

namespace lld::coff {

class DefinedRegular;
class Defined;

struct IncrementalCanonicalEntry {
  std::string symbolName;
  uint32_t offset = 0;
};

bool isIncrementalControlFlowRefKind(IncrementalRefKind kind);
std::optional<IncrementalCanonicalEntry>
findIncrementalCanonicalEntry(const SectionChunk &chunk);
Defined *findIncrementalCanonicalEntrySymbol(const SectionChunk &chunk);
IncrementalRefKind classifyIncrementalAmd64Rel32Ref(
    const SectionChunk &source, const llvm::object::coff_relocation &rel,
    uint32_t targetOffset, bool &redirectEligible);
std::vector<IncrementalEdgeState>
buildIncrementalEdgeStates(COFFLinkerContext &ctx,
                           IncrementalLinkSession &session);

} // namespace lld::coff

#endif
