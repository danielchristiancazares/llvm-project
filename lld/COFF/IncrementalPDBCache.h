//===- IncrementalPDBCache.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INCREMENTALPDBCACHE_H
#define LLD_COFF_INCREMENTALPDBCACHE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/CodeView/DebugSubsection.h"
#include "llvm/DebugInfo/CodeView/TypeHashing.h"
#include "llvm/DebugInfo/CodeView/TypeIndexDiscovery.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lld::coff {

class COFFLinkerContext;
class Configuration;
class ObjFile;
class TpiSource;

enum class IncrementalPDBTypeSourceKind : uint8_t {
  Regular = 1,
  PCH = 2,
  UsingPCH = 3,
  PDB = 4,
};

enum class IncrementalPDBDebugChunkKind : uint8_t {
  DebugS = 1,
  DebugF = 2,
};

enum class IncrementalPDBScopeAction : uint8_t {
  None = 0,
  Open = 1,
  Close = 2,
};

enum IncrementalPDBSymbolDestMask : uint8_t {
  IncrementalPDBGoesToGlobals = 1 << 0,
  IncrementalPDBGoesToModule = 1 << 1,
};

enum IncrementalPDBSymbolPlanFlags : uint8_t {
  IncrementalPDBUsesGlobalProcRef = 1 << 0,
};

struct IncrementalPDBTypeRef {
  llvm::codeview::TiRefKind kind = llvm::codeview::TiRefKind::TypeRef;
  uint32_t offset = 0;
  uint32_t count = 0;
};

struct IncrementalPDBStringFixup {
  uint32_t strTabOffset = 0;
  uint32_t symOffsetOfReference = 0;
};

struct IncrementalPDBSymbolPlan {
  uint32_t recordOffset = 0;
  uint32_t recordLength = 0;
  uint32_t alignedLength = 0;
  uint32_t relocIndex = 0;
  uint32_t typeRefStart = 0;
  uint16_t typeRefCount = 0;
  uint8_t destMask = 0;
  uint8_t flags = 0;
  uint8_t rewriteKind = 0;
  IncrementalPDBScopeAction scopeAction = IncrementalPDBScopeAction::None;
};

struct IncrementalPDBSubsectionPlan {
  llvm::codeview::DebugSubsectionKind kind =
      llvm::codeview::DebugSubsectionKind::None;
  uint32_t recordOffset = 0;
  uint32_t recordLength = 0;
  uint32_t relocIndex = 0;
  uint32_t symbolPlanStart = 0;
  uint32_t symbolPlanCount = 0;
};

struct IncrementalPDBChunkPlan {
  uint32_t chunkOrdinal = 0;
  IncrementalPDBDebugChunkKind kind = IncrementalPDBDebugChunkKind::DebugS;
  uint32_t subsectionStart = 0;
  uint32_t subsectionCount = 0;
};

struct IncrementalPDBModuleCacheEntry {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t debugSHash = 0;
  uint64_t debugFHash = 0;
  uint64_t relocHash = 0;
  uint32_t moduleStreamSize = 0;
  std::vector<IncrementalPDBChunkPlan> chunkPlans;
  std::vector<IncrementalPDBSubsectionPlan> subsectionPlans;
  std::vector<IncrementalPDBSymbolPlan> symbolPlans;
  std::vector<IncrementalPDBTypeRef> typeRefs;
  std::vector<IncrementalPDBStringFixup> stringFixups;
};

struct IncrementalPDBTypeCacheEntry {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  IncrementalPDBTypeSourceKind kind = IncrementalPDBTypeSourceKind::Regular;
  uint64_t contentHash = 0;
  uint64_t dependencyHash = 0;
  uint32_t endPrecompIdx = ~0U;
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
  std::vector<llvm::codeview::GloballyHashedType> auxGHashes;
  std::vector<uint8_t> auxIsItemIndexBits;
};

struct IncrementalPDBCacheFile {
  uint64_t linkerBuildId = 0;
  uint64_t hardConfigHash = 0;
  std::vector<IncrementalPDBTypeCacheEntry> typeEntries;
  std::vector<IncrementalPDBModuleCacheEntry> moduleEntries;
};

enum class IncrementalPDBCacheRuntimeMode : uint8_t {
  BypassCache = 1,
  ReplayOnlyCache = 2,
  RecordOnlyCache = 3,
  ReplayAndRecordCache = 4,
};

class IncrementalPDBCacheSession {
public:
  static std::unique_ptr<IncrementalPDBCacheSession>
  create(COFFLinkerContext &ctx);

  IncrementalPDBCacheSession(const IncrementalPDBCacheSession &) = delete;
  IncrementalPDBCacheSession &
  operator=(const IncrementalPDBCacheSession &) = delete;

  const IncrementalPDBTypeCacheEntry *findTypeEntry(const TpiSource &source);
  const IncrementalPDBModuleCacheEntry *findModuleEntry(const ObjFile &file);

  void recordTypeEntries(llvm::ArrayRef<TpiSource *> sources);
  llvm::Error writeCache(const llvm::DenseMap<const ObjFile *,
                                              IncrementalPDBModuleCacheEntry>
                             &modulePlans) const;

  IncrementalPDBCacheRuntimeMode runtimeMode() const { return mode; }
  uint64_t getTypeCacheHits() const { return typeCacheHits; }
  uint64_t getTypeCacheMisses() const { return typeCacheMisses; }
  uint64_t getModuleCacheHits() const { return moduleCacheHits; }
  uint64_t getModuleCacheMisses() const { return moduleCacheMisses; }

private:
  explicit IncrementalPDBCacheSession(COFFLinkerContext &ctx);

  const IncrementalPDBTypeCacheEntry *
  findLoadedTypeEntry(llvm::StringRef key, const TpiSource &source) const;
  const IncrementalPDBModuleCacheEntry *
  findLoadedModuleEntry(llvm::StringRef key, const ObjFile &file) const;

  COFFLinkerContext &ctx;
  llvm::SmallString<128> cachePath;
  IncrementalPDBCacheRuntimeMode mode =
      IncrementalPDBCacheRuntimeMode::BypassCache;
  IncrementalPDBCacheFile loadedCache;
  llvm::StringMap<const IncrementalPDBTypeCacheEntry *> loadedTypesByKey;
  llvm::StringMap<const IncrementalPDBModuleCacheEntry *> loadedModulesByKey;
  std::vector<IncrementalPDBTypeCacheEntry> recordedTypeEntries;
  uint64_t typeCacheHits = 0;
  uint64_t typeCacheMisses = 0;
  uint64_t moduleCacheHits = 0;
  uint64_t moduleCacheMisses = 0;
};

llvm::SmallString<128>
getIncrementalPDBCachePath(const Configuration &config);

IncrementalPDBCacheRuntimeMode
classifyIncrementalPDBCacheRuntimeMode(const COFFLinkerContext &ctx);
uint64_t computeIncrementalPDBCacheBuildId();
uint64_t computeIncrementalPDBCacheHardConfigHash(const Configuration &config);
llvm::Expected<IncrementalPDBCacheFile>
loadIncrementalPDBCache(llvm::StringRef path);
llvm::Error writeIncrementalPDBCache(llvm::StringRef path,
                                     const IncrementalPDBCacheFile &cache);

std::string getIncrementalPDBCacheObjectKey(const ObjFile &file);
uint64_t computeIncrementalPDBModuleDebugSHash(const ObjFile &file);
uint64_t computeIncrementalPDBModuleDebugFHash(const ObjFile &file);
uint64_t computeIncrementalPDBModuleRelocHash(const ObjFile &file);

} // namespace lld::coff

#endif
