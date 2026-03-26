//===- PDB.h ----------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_PDB_H
#define LLD_COFF_PDB_H

#include "lld/Common/Closed.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <optional>
#include <utility>

namespace llvm::codeview {
union DebugInfo;
}

namespace lld {
class Timer;

namespace coff {
class SectionChunk;
class COFFLinkerContext;

void createPDB(COFFLinkerContext &ctx, llvm::ArrayRef<uint8_t> sectionTable,
               llvm::codeview::DebugInfo *buildId);

std::optional<std::pair<llvm::StringRef, uint32_t>>
getFileLineCodeView(const SectionChunk *c, uint32_t addr);

// For statistics
struct PDBStats {
  uint64_t globalSymbols = 0;
  uint64_t moduleSymbols = 0;
  uint64_t publicSymbols = 0;
  uint64_t nbTypeRecords = 0;
  uint64_t nbTypeRecordsBytes = 0;
  uint64_t nbTPIrecords = 0;
  uint64_t nbIPIrecords = 0;
  uint64_t strTabSize = 0;
  std::string largeInputTypeRecs;
};

struct PrintZeroedPDBSummary final {};

struct PrintMeasuredPDBSummary final {
  PDBStats stats;
};

class PDBSummary final {
public:
  PDBSummary() : storage(makeZeroedStorage()) {}
  PDBSummary(const PDBSummary &) = delete;
  PDBSummary &operator=(const PDBSummary &) = delete;
  PDBSummary(PDBSummary &&) = delete;
  PDBSummary &operator=(PDBSummary &&) = delete;

  void enableMeasuredRows() { storage = makeMeasuredStorage(); }

  template <class Fn> void withMeasuredStats(Fn &&fn) {
    storage->match(
        [](PrintZeroedPDBSummary &) {},
        [&](PrintMeasuredPDBSummary &summary) {
          std::forward<Fn>(fn)(summary.stats);
        });
  }

  template <class... Fs> decltype(auto) match(Fs &&...fns) & {
    return storage->match(std::forward<Fs>(fns)...);
  }

  template <class... Fs> decltype(auto) match(Fs &&...fns) const & {
    return storage->match(std::forward<Fs>(fns)...);
  }

private:
  using Storage = lld::Closed<PrintZeroedPDBSummary, PrintMeasuredPDBSummary>;

  static std::unique_ptr<Storage> makeZeroedStorage() {
    return std::make_unique<Storage>(Storage::make<PrintZeroedPDBSummary>());
  }

  static std::unique_ptr<Storage> makeMeasuredStorage() {
    return std::make_unique<Storage>(Storage::make<PrintMeasuredPDBSummary>());
  }

  std::unique_ptr<Storage> storage;
};

} // namespace coff
} // namespace lld

#endif
