#ifndef LLD_COFF_INCREMENTALLAYOUT_H
#define LLD_COFF_INCREMENTALLAYOUT_H

#include "lld/Common/Closed.h"
#include <cstdint>

namespace lld::coff {

class COFFLinkerContext;

struct IncrementalLayoutResult {
  uint64_t fileSize = 0;
  uint64_t sizeOfImage = 0;
  uint64_t sizeOfHeaders = 0;
};

struct WriteCurrentFullImageLayout final {};
struct RecomputeCurrentFullImageLayout final {};
struct WriteReusedIncrementalLayout final {
  IncrementalLayoutResult layout;
};
using IncrementalLayoutOutcome =
    lld::Closed<WriteCurrentFullImageLayout, RecomputeCurrentFullImageLayout,
                WriteReusedIncrementalLayout>;

IncrementalLayoutOutcome applyIncrementalLayout(COFFLinkerContext &ctx);

} // namespace lld::coff

#endif
