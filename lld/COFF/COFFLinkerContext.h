//===- COFFLinkerContext.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_COFFLINKERCONTEXT_H
#define LLD_COFF_COFFLINKERCONTEXT_H

#include "Chunks.h"
#include "Config.h"
#include "DebugTypes.h"
#include "Driver.h"
#include "Incremental.h"
#include "InputFiles.h"
#include "PDB.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Timer.h"
#include <cstdint>

namespace lld::coff {

class IncrementalPDBCacheSession;

struct SymbolInsertStats {
  uint64_t calls = 0;
  uint64_t inserted = 0;
  uint64_t existing = 0;

  void mergeFrom(const SymbolInsertStats &other) {
    calls += other.calls;
    inserted += other.inserted;
    existing += other.existing;
  }
};

struct SymbolUndefinedStats {
  uint64_t calls = 0;
  uint64_t newOrOverrode = 0;
  uint64_t forcedLazy = 0;
  uint64_t reused = 0;

  void mergeFrom(const SymbolUndefinedStats &other) {
    calls += other.calls;
    newOrOverrode += other.newOrOverrode;
    forcedLazy += other.forcedLazy;
    reused += other.reused;
  }
};

struct SymbolRegularStats {
  uint64_t calls = 0;
  uint64_t newOrReplaced = 0;
  uint64_t duplicate = 0;
  uint64_t ignoredWeak = 0;

  void mergeFrom(const SymbolRegularStats &other) {
    calls += other.calls;
    newOrReplaced += other.newOrReplaced;
    duplicate += other.duplicate;
    ignoredWeak += other.ignoredWeak;
  }
};

struct SymbolComdatStats {
  uint64_t calls = 0;
  uint64_t inserted = 0;
  uint64_t existingComdat = 0;
  uint64_t duplicateNonComdat = 0;

  void mergeFrom(const SymbolComdatStats &other) {
    calls += other.calls;
    inserted += other.inserted;
    existingComdat += other.existingComdat;
    duplicateNonComdat += other.duplicateNonComdat;
  }
};

struct SymbolCommonStats {
  uint64_t calls = 0;
  uint64_t newOrReplacedNonCOFF = 0;
  uint64_t replacedLarger = 0;
  uint64_t reusedExisting = 0;

  void mergeFrom(const SymbolCommonStats &other) {
    calls += other.calls;
    newOrReplacedNonCOFF += other.newOrReplacedNonCOFF;
    replacedLarger += other.replacedLarger;
    reusedExisting += other.reusedExisting;
  }
};

struct SymbolMutationStats {
  SymbolInsertStats insert;
  SymbolUndefinedStats addUndefined;
  SymbolRegularStats addRegular;
  SymbolComdatStats addComdat;
  SymbolCommonStats addCommon;

  void mergeFrom(const SymbolMutationStats &other) {
    insert.mergeFrom(other.insert);
    addUndefined.mergeFrom(other.addUndefined);
    addRegular.mergeFrom(other.addRegular);
    addComdat.mergeFrom(other.addComdat);
    addCommon.mergeFrom(other.addCommon);
  }
};

class COFFLinkerContext : public CommonLinkerContext {
public:
  COFFLinkerContext();
  COFFLinkerContext(const COFFLinkerContext &) = delete;
  COFFLinkerContext &operator=(const COFFLinkerContext &) = delete;
  ~COFFLinkerContext();

  LinkerDriver driver;
  SymbolTable symtab;
  COFFOptTable optTable;

  // A native ARM64 symbol table on ARM64X target.
  std::optional<SymbolTable> hybridSymtab;

  // Returns the appropriate symbol table for the specified machine type.
  SymbolTable &getSymtab(llvm::COFF::MachineTypes machine) {
    if (hybridSymtab && machine == ARM64)
      return *hybridSymtab;
    return symtab;
  }

  // Invoke the specified callback for each symbol table.
  void forEachSymtab(std::function<void(SymbolTable &symtab)> f) {
    // If present, process the native symbol table first.
    if (hybridSymtab)
      f(*hybridSymtab);
    f(symtab);
  }

  // Invoke the specified callback for each active symbol table,
  // skipping the native symbol table on pure ARM64EC targets.
  void forEachActiveSymtab(std::function<void(SymbolTable &symtab)> f) {
    if (symtab.ctx.config.machine == ARM64X)
      f(*hybridSymtab);
    f(symtab);
  }

  // Invoke the specified callback for each symbol table that loaded inputs.
  void forEachSymtabWithInputs(std::function<void(SymbolTable &symtab)> f) {
    if (hybridSymtab && hybridSymtab->hasInputFiles())
      f(*hybridSymtab);
    if (symtab.hasInputFiles())
      f(symtab);
  }

  std::vector<ObjFile *> objFileInstances;
  std::vector<ArchiveFile *> archiveFileInstances;
  std::map<std::string, PDBInputFile *> pdbInputFileInstances;
  std::vector<ImportFile *> importFileInstances;
  std::int64_t consumedInputsSize = 0;

  MergeChunk *mergeChunkInstances[Log2MaxSectionAlignment + 1] = {};

  /// All sources of type information in the program.
  std::vector<TpiSource *> tpiSourceList;

  void addTpiSource(TpiSource *tpi) { tpiSourceList.push_back(tpi); }

  std::map<llvm::codeview::GUID, TpiSource *> typeServerSourceMappings;
  std::map<uint32_t, TpiSource *> precompSourceMappings;

  /// List of all output sections. After output sections are finalized, this
  /// can be indexed by getOutputSection.
  std::vector<OutputSection *> outputSections;

  OutputSection *getOutputSection(const Chunk *c) const {
    return c->osidx == 0 ? nullptr : outputSections[c->osidx - 1];
  }

  // Fake sections for parsing bitcode files.
  FakeSection ltoTextSection;
  FakeSection ltoDataSection;
  FakeSectionChunk ltoTextSectionChunk;
  FakeSectionChunk ltoDataSectionChunk;

  // All timers used in the COFF linker.
  Timer rootTimer;
  Timer inputFileTimer;
  Timer inputParseTimer;
  Timer initializeChunksTimer;
  Timer initializeSymbolsTimer;
  Timer initializeSymbolsMainPassTimer;
  Timer initializeSymbolsUndefinedTimer;
  Timer initializeSymbolsWeakExternalsTimer;
  Timer initializeSymbolsDefinedTimer;
  Timer initializeSymbolsPendingDeferralTimer;
  Timer initializeSymbolsCommonTimer;
  Timer initializeSymbolsAbsoluteTimer;
  Timer initializeSymbolsEmptySectionsTimer;
  Timer initializeSymbolsComdatTimer;
  Timer initializeSymbolsRegularTimer;
  Timer initializeSymbolsPendingTimer;
  Timer initializeSymbolsWeakAliasesTimer;
  Timer symbolTableInsertTimer;
  Timer initializeFlagsTimer;
  Timer initializeDependenciesTimer;
  Timer initializeECThunksTimer;
  Timer ltoTimer;
  Timer gcTimer;
  Timer icfTimer;
  Timer incrementalStateReadTimer;
  Timer incrementalOutputVerifyTimer;
  Timer incrementalInputHashTimer;
  Timer incrementalSymbolValidationTimer;
  Timer incrementalLayoutTimer;
  Timer incrementalStateBuildTimer;
  Timer incrementalStateWriteTimer;

  // Writer timers.
  Timer codeLayoutTimer;
  Timer outputCommitTimer;
  Timer totalMapTimer;
  Timer symbolGatherTimer;
  Timer symbolStringsTimer;
  Timer writeTimer;

  // PDB timers.
  Timer totalPdbLinkTimer;
  Timer addObjectsTimer;
  Timer typeMergingTimer;
  Timer loadGHashTimer;
  Timer mergeGHashTimer;
  Timer pdbCacheLoadTimer;
  Timer pdbCacheValidateTimer;
  Timer pdbTypeCacheReplayTimer;
  Timer symbolMergingTimer;
  Timer handleDebugSTimer;
  Timer pdbModulePlanReplayTimer;
  Timer globalSymbolRecordWriteTimer;
  Timer globalSymbolRelocateTimer;
  Timer globalSymbolTypeRemapTimer;
  Timer globalSymbolIdTranslateTimer;
  Timer publicsLayoutTimer;
  Timer tpiStreamLayoutTimer;
  Timer diskCommitTimer;
  Timer pdbCacheStoreTimer;
  Timer commitModuleSymbolsTimer;
  Timer moduleSymbolRecordWriteTimer;
  Timer moduleSymbolRelocateTimer;
  Timer moduleSymbolTypeRemapTimer;
  Timer moduleSymbolIdTranslateTimer;

  PDBSummary pdbSummary;

  Configuration config;
  std::unique_ptr<IncrementalCoordinator> incremental;
  std::unique_ptr<IncrementalPDBCacheSession> pdbCacheSession;
  llvm::StringSet<> loadedArchiveMemberKeys;

  DynamicRelocsChunk *dynamicRelocs = nullptr;

  SymbolMutationStats symbolMutationStats;

  void printSymbolMutationStats(llvm::raw_ostream &os) const;
};

} // namespace lld::coff

#endif
