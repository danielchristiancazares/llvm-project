//===- COFFContext.cpp ----------------------------------------------------===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Description
//
//===----------------------------------------------------------------------===//

#include "COFFLinkerContext.h"
#include "Incremental.h"
#include "IncrementalPDBCache.h"
#include "Symbols.h"
#include "llvm/BinaryFormat/COFF.h"

namespace lld::coff {
COFFLinkerContext::COFFLinkerContext()
    : driver(*this), symtab(*this),
      incremental(IncrementalCoordinator::makeDisabled()),
      ltoTextSection(llvm::COFF::IMAGE_SCN_MEM_EXECUTE),
      ltoDataSection(llvm::COFF::IMAGE_SCN_CNT_INITIALIZED_DATA),
      ltoTextSectionChunk(&ltoTextSection.section),
      ltoDataSectionChunk(&ltoDataSection.section),
      rootTimer("Total Linking Time"),
      inputFileTimer("Input File Reading", rootTimer),
      inputParseTimer("Input Parse", inputFileTimer),
      initializeChunksTimer("Initialize Chunks", inputParseTimer),
      initializeSymbolsTimer("Initialize Symbols", inputParseTimer),
      initializeSymbolsMainPassTimer("Main Symbol Scan",
                                     initializeSymbolsTimer),
      initializeSymbolsPendingTimer("Resolve Pending Symbols",
                                    initializeSymbolsTimer),
      initializeSymbolsWeakAliasesTimer("Resolve Weak Aliases",
                                        initializeSymbolsTimer),
      initializeFlagsTimer("Initialize Flags", inputParseTimer),
      initializeDependenciesTimer("Initialize Dependencies", inputParseTimer),
      initializeECThunksTimer("Initialize EC Thunks", inputParseTimer),
      ltoTimer("LTO", rootTimer), gcTimer("GC", rootTimer),
      icfTimer("ICF", rootTimer),
      incrementalStateReadTimer("Load Incremental State", rootTimer),
      incrementalOutputVerifyTimer("Verify Prior Output", rootTimer),
      incrementalInputHashTimer("Hash Incremental Inputs", rootTimer),
      incrementalSymbolValidationTimer("Validate Incremental Symbols",
                                       rootTimer),
      incrementalLayoutTimer("Apply Incremental Layout", rootTimer),
      incrementalStateBuildTimer("Rebuild Incremental Snapshot", rootTimer),
      incrementalStateWriteTimer("Write Incremental State", rootTimer),
      codeLayoutTimer("Code Layout", rootTimer),
      outputCommitTimer("Commit Output File", rootTimer),
      totalMapTimer("MAP Emission (Cumulative)", rootTimer),
      symbolGatherTimer("Gather Symbols", totalMapTimer),
      symbolStringsTimer("Build Symbol Strings", totalMapTimer),
      writeTimer("Write to File", totalMapTimer),
      totalPdbLinkTimer("PDB Emission (Cumulative)", rootTimer),
      addObjectsTimer("Add Objects", totalPdbLinkTimer),
      typeMergingTimer("Type Merging", addObjectsTimer),
      loadGHashTimer("Global Type Hashing", addObjectsTimer),
      mergeGHashTimer("GHash Type Merging", addObjectsTimer),
      pdbCacheLoadTimer("Load Incremental PDB Cache", addObjectsTimer),
      pdbCacheValidateTimer("Validate Incremental PDB Cache", addObjectsTimer),
      pdbTypeCacheReplayTimer("Replay Incremental Type Cache", addObjectsTimer),
      symbolMergingTimer("Symbol Merging", addObjectsTimer),
      handleDebugSTimer("Handle .debug$S", symbolMergingTimer),
      pdbModulePlanReplayTimer("Replay Incremental Module Plans",
                               symbolMergingTimer),
      globalSymbolRecordWriteTimer("Rewrite Global Symbols",
                                   handleDebugSTimer),
      globalSymbolRelocateTimer("Relocate Global Symbols",
                                globalSymbolRecordWriteTimer),
      globalSymbolTypeRemapTimer("Remap Global Symbol Types",
                                 globalSymbolRecordWriteTimer),
      globalSymbolIdTranslateTimer("Translate Global IDs",
                                   globalSymbolRecordWriteTimer),
      publicsLayoutTimer("Publics Stream Layout", totalPdbLinkTimer),
      tpiStreamLayoutTimer("TPI Stream Layout", totalPdbLinkTimer),
      diskCommitTimer("Commit to Disk", totalPdbLinkTimer),
      pdbCacheStoreTimer("Store Incremental PDB Cache", totalPdbLinkTimer),
      commitModuleSymbolsTimer("Commit Module Symbols", diskCommitTimer),
      moduleSymbolRecordWriteTimer("Rewrite Module Symbols",
                                   commitModuleSymbolsTimer),
      moduleSymbolRelocateTimer("Relocate Module Symbols",
                                moduleSymbolRecordWriteTimer),
      moduleSymbolTypeRemapTimer("Remap Module Symbol Types",
                                 moduleSymbolRecordWriteTimer),
      moduleSymbolIdTranslateTimer("Translate Module IDs",
                                   moduleSymbolRecordWriteTimer) {}

COFFLinkerContext::~COFFLinkerContext() = default;
} // namespace lld::coff
