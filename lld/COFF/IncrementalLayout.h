#ifndef LLD_COFF_INCREMENTALLAYOUT_H
#define LLD_COFF_INCREMENTALLAYOUT_H

#include <cstdint>

namespace lld::coff {

class COFFLinkerContext;

struct IncrementalLayoutResult {
  uint64_t fileSize = 0;
  uint64_t sizeOfImage = 0;
  uint64_t sizeOfHeaders = 0;
};

bool applyIncrementalLayout(COFFLinkerContext &ctx,
                            IncrementalLayoutResult &result);

} // namespace lld::coff

#endif
