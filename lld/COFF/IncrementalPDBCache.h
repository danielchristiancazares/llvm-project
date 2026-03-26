//===- IncrementalPDBCache.h -----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_COFF_INCREMENTALPDBCACHE_H
#define LLD_COFF_INCREMENTALPDBCACHE_H

#include "lld/Common/Closed.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/DebugInfo/CodeView/DebugSubsection.h"
#include "llvm/DebugInfo/CodeView/TypeHashing.h"
#include "llvm/DebugInfo/CodeView/TypeIndexDiscovery.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lld::coff {

class COFFLinkerContext;
class Configuration;
class ObjFile;
class TpiSource;

struct IncrementalPDBTypeRef {
  llvm::codeview::TiRefKind kind = llvm::codeview::TiRefKind::TypeRef;
  uint32_t offset = 0;
  uint32_t count = 0;
};

struct IncrementalPDBStringFixup {
  uint32_t strTabOffset = 0;
  uint32_t symOffsetOfReference = 0;
};

struct ReplayAllTypeRecords {};

struct ReplayTypeRecordsSkippingEndPrecomp {
  uint32_t ghashIndex = 0;
};

using IncrementalPDBTypeReplayBoundary =
    Closed<ReplayAllTypeRecords, ReplayTypeRecordsSkippingEndPrecomp>;

struct ReplayObjectTypes {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  IncrementalPDBTypeReplayBoundary boundary =
      IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
};

struct ReplayPrecompiledHeaderTypes {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  uint32_t pchSignature = 0;
  IncrementalPDBTypeReplayBoundary boundary =
      IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
};

struct ReplayUsingPrecompiledHeaderTypes {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  uint64_t dependencyHash = 0;
  IncrementalPDBTypeReplayBoundary boundary =
      IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
};

struct ReplayTypeServerTpiOnly {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  IncrementalPDBTypeReplayBoundary boundary =
      IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
};

struct ReplayTypeServerTpiAndIpi {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t contentHash = 0;
  IncrementalPDBTypeReplayBoundary boundary =
      IncrementalPDBTypeReplayBoundary::make<ReplayAllTypeRecords>();
  std::vector<llvm::codeview::GloballyHashedType> ghashes;
  std::vector<uint8_t> isItemIndexBits;
  std::vector<llvm::codeview::GloballyHashedType> auxGHashes;
  std::vector<uint8_t> auxIsItemIndexBits;
};

using IncrementalPDBTypeReplaySnapshot =
    Closed<ReplayObjectTypes, ReplayPrecompiledHeaderTypes,
           ReplayUsingPrecompiledHeaderTypes, ReplayTypeServerTpiOnly,
           ReplayTypeServerTpiAndIpi>;

struct IncrementalPDBRecordLocation {
  uint32_t recordOffset = 0;
  uint32_t recordLength = 0;
  uint32_t relocIndex = 0;
};

struct EmitGlobalOnlySymbol {};

struct EmitModuleOnlySymbol {};

struct EmitGlobalAndModuleSymbol {};

using SymbolReplayRouting =
    Closed<EmitGlobalOnlySymbol, EmitModuleOnlySymbol, EmitGlobalAndModuleSymbol>;

struct OmitGlobalReplay {};

struct ReplayGlobalSymbolBytes {};

struct ReplayGlobalProcedureReference {};

using GlobalSymbolReplay = Closed<OmitGlobalReplay, ReplayGlobalSymbolBytes,
                                  ReplayGlobalProcedureReference>;

struct ReplaySymbolWithoutTypeRewrite {};

struct ReplayProcIdEndSymbol {};

struct ReplayProcIdWithFixedTypeIndex {};

struct ReplaySymbolWithDiscoveredTypeRefs {
  std::vector<IncrementalPDBTypeRef> typeRefs;
};

using SymbolRewritePlan =
    Closed<ReplaySymbolWithoutTypeRewrite, ReplayProcIdEndSymbol,
           ReplayProcIdWithFixedTypeIndex, ReplaySymbolWithDiscoveredTypeRefs>;

struct ReplayStandaloneSymbol {};

struct ReplayScopeOpeningSymbol {};

struct ReplayScopeClosingSymbol {};

using SymbolScopeReplay =
    Closed<ReplayStandaloneSymbol, ReplayScopeOpeningSymbol,
           ReplayScopeClosingSymbol>;

struct CachedSymbolReplay {
  IncrementalPDBRecordLocation location;
  uint32_t alignedLength = 0;
  SymbolReplayRouting routing =
      SymbolReplayRouting::make<EmitModuleOnlySymbol>();
  GlobalSymbolReplay globalReplay =
      GlobalSymbolReplay::make<OmitGlobalReplay>();
  SymbolRewritePlan rewrite =
      SymbolRewritePlan::make<ReplaySymbolWithoutTypeRewrite>();
  SymbolScopeReplay scope =
      SymbolScopeReplay::make<ReplayStandaloneSymbol>();
};

struct ReplayOpaqueSubsection {
  llvm::codeview::DebugSubsectionKind kind =
      llvm::codeview::DebugSubsectionKind::None;
  IncrementalPDBRecordLocation location;
};

struct ReplaySymbolSubsection {
  IncrementalPDBRecordLocation location;
  std::vector<CachedSymbolReplay> symbols;
};

using IncrementalPDBSubsectionReplay =
    Closed<ReplayOpaqueSubsection, ReplaySymbolSubsection>;

struct ReplayDebugSChunk {
  uint32_t chunkOrdinal = 0;
  std::vector<IncrementalPDBSubsectionReplay> subsections;
};

struct ReplayDebugFChunk {
  uint32_t chunkOrdinal = 0;
};

using IncrementalPDBChunkReplay =
    Closed<ReplayDebugSChunk, ReplayDebugFChunk>;

struct CachedModuleReplay {
  std::string path;
  std::string parentPath;
  uint64_t archiveOffset = 0;
  uint64_t debugSHash = 0;
  uint64_t debugFHash = 0;
  uint64_t relocHash = 0;
  uint32_t moduleStreamSize = 0;
  std::vector<IncrementalPDBChunkReplay> chunks;
  std::vector<IncrementalPDBStringFixup> stringFixups;
};

struct IncrementalPDBCacheSnapshot {
  uint64_t linkerBuildId = 0;
  uint64_t hardConfigHash = 0;
  std::vector<IncrementalPDBTypeReplaySnapshot> typeReplays;
  std::vector<CachedModuleReplay> moduleReplays;
};

CachedModuleReplay cloneCachedModuleReplay(const CachedModuleReplay &entry);

struct ReplayTypeFromCache {
  std::reference_wrapper<const IncrementalPDBTypeReplaySnapshot> replay;
};

struct RebuildTypeFromCurrentInput {};

using IncrementalPDBTypeReplayLookup =
    Closed<ReplayTypeFromCache, RebuildTypeFromCurrentInput>;

struct ReplayModuleFromCache {
  std::reference_wrapper<const CachedModuleReplay> replay;
};

struct RebuildModuleFromCurrentInput {};

using IncrementalPDBModuleReplayLookup =
    Closed<ReplayModuleFromCache, RebuildModuleFromCurrentInput>;

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

  IncrementalPDBTypeReplayLookup lookupTypeReplay(const TpiSource &source);
  IncrementalPDBModuleReplayLookup lookupModuleReplay(const ObjFile &file);

  void recordTypeEntries(llvm::ArrayRef<TpiSource *> sources);
  llvm::Error writeCache(const llvm::DenseMap<const ObjFile *,
                                              CachedModuleReplay> &modulePlans) const;

  IncrementalPDBCacheRuntimeMode runtimeMode() const { return mode; }
  uint64_t getTypeCacheHits() const { return typeCacheHits; }
  uint64_t getTypeCacheMisses() const { return typeCacheMisses; }
  uint64_t getModuleCacheHits() const { return moduleCacheHits; }
  uint64_t getModuleCacheMisses() const { return moduleCacheMisses; }

private:
  explicit IncrementalPDBCacheSession(COFFLinkerContext &ctx);

  const IncrementalPDBTypeReplaySnapshot *
  findLoadedTypeReplay(llvm::StringRef key, const TpiSource &source) const;
  const CachedModuleReplay *
  findLoadedModuleReplay(llvm::StringRef key, const ObjFile &file) const;

  COFFLinkerContext &ctx;
  llvm::SmallString<128> cachePath;
  IncrementalPDBCacheRuntimeMode mode =
      IncrementalPDBCacheRuntimeMode::BypassCache;
  IncrementalPDBCacheSnapshot loadedCache;
  llvm::StringMap<const IncrementalPDBTypeReplaySnapshot *> loadedTypesByKey;
  llvm::StringMap<const CachedModuleReplay *> loadedModulesByKey;
  std::vector<IncrementalPDBTypeReplaySnapshot> recordedTypeReplays;
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
llvm::Expected<IncrementalPDBCacheSnapshot>
loadIncrementalPDBCache(llvm::StringRef path);
llvm::Error writeIncrementalPDBCache(llvm::StringRef path,
                                     const IncrementalPDBCacheSnapshot &cache);

std::string getIncrementalPDBCacheObjectKey(const ObjFile &file);
uint64_t computeIncrementalPDBModuleDebugSHash(const ObjFile &file);
uint64_t computeIncrementalPDBModuleDebugFHash(const ObjFile &file);
uint64_t computeIncrementalPDBModuleRelocHash(const ObjFile &file);

} // namespace lld::coff

#endif
