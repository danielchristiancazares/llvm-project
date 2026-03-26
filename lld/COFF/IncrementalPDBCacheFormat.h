//===- IncrementalPDBCacheFormat.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INCREMENTALPDBCACHEFORMAT_H
#define LLD_COFF_INCREMENTALPDBCACHEFORMAT_H

#include "llvm/Support/Endian.h"
#include <cstdint>

namespace lld::coff {

using llvm::support::ulittle16_t;
using llvm::support::ulittle32_t;
using llvm::support::ulittle64_t;

constexpr char incrementalPDBCacheMagic[8] = {'L', 'L', 'P', 'D',
                                              'B', 'C', '6', '4'};
constexpr uint32_t incrementalPDBCacheVersion = 2;
constexpr uint16_t incrementalPDBCacheFlagHasTypeEntries = 1u << 0;
constexpr uint16_t incrementalPDBCacheFlagHasModuleEntries = 1u << 1;

struct IncrementalPDBBlobRefRecord {
  ulittle64_t offset;
  ulittle64_t size;
};

struct IncrementalPDBCacheHeader {
  char magic[8];
  ulittle32_t version;
  ulittle16_t machine;
  ulittle16_t flags;
  ulittle64_t linkerBuildId;
  ulittle64_t hardConfigHash;
  ulittle64_t stringTableOffset;
  ulittle64_t stringTableSize;
  ulittle64_t typeTableOffset;
  ulittle32_t typeCount;
  ulittle32_t reserved0;
  ulittle64_t moduleTableOffset;
  ulittle32_t moduleCount;
  ulittle32_t reserved1;
  ulittle64_t chunkPlanTableOffset;
  ulittle32_t chunkPlanCount;
  ulittle32_t reserved2;
  ulittle64_t subsectionPlanTableOffset;
  ulittle32_t subsectionPlanCount;
  ulittle32_t reserved3;
  ulittle64_t symbolPlanTableOffset;
  ulittle32_t symbolPlanCount;
  ulittle32_t reserved4;
  ulittle64_t typeRefTableOffset;
  ulittle32_t typeRefCount;
  ulittle32_t reserved5;
  ulittle64_t stringFixupTableOffset;
  ulittle32_t stringFixupCount;
  ulittle32_t reserved6;
  ulittle64_t blobOffset;
  ulittle64_t blobSize;
};

struct IncrementalPDBTypeEntryRecord {
  ulittle64_t pathOffset;
  ulittle64_t parentPathOffset;
  ulittle64_t archiveOffset;
  uint8_t kind;
  uint8_t reserved0;
  ulittle16_t reserved1;
  ulittle64_t contentHash;
  ulittle64_t dependencyHash;
  ulittle32_t endPrecompIdx;
  ulittle32_t reserved2;
  IncrementalPDBBlobRefRecord ghashes;
  IncrementalPDBBlobRefRecord isItemIndexBits;
  IncrementalPDBBlobRefRecord auxGHashes;
  IncrementalPDBBlobRefRecord auxIsItemIndexBits;
};

struct IncrementalPDBModuleEntryRecord {
  ulittle64_t pathOffset;
  ulittle64_t parentPathOffset;
  ulittle64_t archiveOffset;
  ulittle64_t debugSHash;
  ulittle64_t debugFHash;
  ulittle64_t relocHash;
  ulittle32_t moduleStreamSize;
  ulittle32_t reserved0;
  ulittle32_t chunkPlanStart;
  ulittle32_t chunkPlanCount;
  ulittle32_t subsectionPlanStart;
  ulittle32_t subsectionPlanCount;
  ulittle32_t symbolPlanStart;
  ulittle32_t symbolPlanCount;
  ulittle32_t typeRefStart;
  ulittle32_t typeRefCount;
  ulittle32_t stringFixupStart;
  ulittle32_t stringFixupCount;
};

struct IncrementalPDBChunkPlanRecord {
  ulittle32_t chunkOrdinal;
  uint8_t kind;
  uint8_t reserved0;
  ulittle16_t reserved1;
  ulittle32_t subsectionStart;
  ulittle32_t subsectionCount;
};

struct IncrementalPDBSubsectionPlanRecord {
  ulittle16_t kind;
  ulittle16_t reserved0;
  ulittle32_t recordOffset;
  ulittle32_t recordLength;
  ulittle32_t relocIndex;
  ulittle32_t symbolPlanStart;
  ulittle32_t symbolPlanCount;
};

struct IncrementalPDBSymbolPlanRecord {
  ulittle32_t recordOffset;
  ulittle32_t recordLength;
  ulittle32_t alignedLength;
  ulittle32_t relocIndex;
  ulittle32_t typeRefStart;
  ulittle16_t typeRefCount;
  uint8_t destMask;
  uint8_t flags;
  uint8_t rewriteKind;
  uint8_t scopeAction;
  ulittle16_t reserved0;
};

struct IncrementalPDBTypeRefRecord {
  uint8_t kind;
  uint8_t reserved0;
  ulittle16_t reserved1;
  ulittle32_t offset;
  ulittle32_t count;
};

struct IncrementalPDBStringFixupRecord {
  ulittle32_t strTabOffset;
  ulittle32_t symOffsetOfReference;
};

} // namespace lld::coff

#endif
