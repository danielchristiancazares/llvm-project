#ifndef LLD_COFF_INCREMENTALREDIRECTS_H
#define LLD_COFF_INCREMENTALREDIRECTS_H

#include "Incremental.h"
#include <vector>

namespace lld::coff {

class DefinedRegular;
class Defined;

struct IncrementalCanonicalEntry {
  std::string symbolName;
  uint32_t offset = 0;
};

Defined *findIncrementalCanonicalEntrySymbol(const SectionChunk &chunk);
std::vector<IncrementalEdgeState>
buildIncrementalEdgeStates(COFFLinkerContext &ctx,
                           const IncrementalInputIndexMap &inputIndices);

} // namespace lld::coff

#endif
