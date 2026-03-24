#include "../../COFF/Config.h"
#include "../../COFF/IncrementalPDBCache.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstring>

using namespace lld::coff;
using namespace llvm;

namespace {

class IncrementalPDBCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_FALSE(
        sys::fs::createUniqueDirectory("lld-incremental-pdbcache", testDir));
  }

  void TearDown() override {
    std::error_code ec = sys::fs::remove_directories(testDir);
    EXPECT_FALSE(ec);
  }

  SmallString<128> getPath(StringRef fileName) const {
    SmallString<128> path(testDir);
    sys::path::append(path, fileName);
    return path;
  }

  static void expectNoError(Error err) {
    if (!err)
      return;
    FAIL() << toString(std::move(err));
  }

  SmallString<128> testDir;
};

static codeview::GloballyHashedType makeHash(uint8_t fill) {
  codeview::GloballyHashedType hash = {};
  memset(&hash, fill, sizeof(hash));
  return hash;
}

TEST_F(IncrementalPDBCacheTest, RoundTripPreservesTypeAndModulePlans) {
  IncrementalPDBCacheFile cache;
  cache.linkerBuildId = 0x1111222233334444ULL;
  cache.hardConfigHash = 0x5555666677778888ULL;

  IncrementalPDBTypeCacheEntry typeEntry;
  typeEntry.path = "main.obj";
  typeEntry.parentPath = "archive.lib";
  typeEntry.archiveOffset = 42;
  typeEntry.kind = IncrementalPDBTypeSourceKind::PDB;
  typeEntry.contentHash = 0xAAAABBBBCCCCDDDDULL;
  typeEntry.dependencyHash = 0x1011121314151617ULL;
  typeEntry.endPrecompIdx = 17;
  typeEntry.ghashes.push_back(makeHash(0x11));
  typeEntry.isItemIndexBits = {0x05};
  typeEntry.auxGHashes.push_back(makeHash(0x22));
  typeEntry.auxIsItemIndexBits = {0x01};
  cache.typeEntries.push_back(typeEntry);

  IncrementalPDBModuleCacheEntry moduleEntry;
  moduleEntry.path = "main.obj";
  moduleEntry.parentPath = "archive.lib";
  moduleEntry.archiveOffset = 42;
  moduleEntry.debugSHash = 0x100;
  moduleEntry.debugFHash = 0x200;
  moduleEntry.relocHash = 0x300;
  moduleEntry.moduleStreamSize = 64;
  moduleEntry.chunkPlans.push_back(
      {1, IncrementalPDBDebugChunkKind::DebugS, 0, 2});
  moduleEntry.subsectionPlans.push_back(
      {codeview::DebugSubsectionKind::StringTable, 4, 8, 0, 0, 0});
  moduleEntry.subsectionPlans.push_back(
      {codeview::DebugSubsectionKind::Symbols, 12, 16, 1, 0, 1});
  moduleEntry.symbolPlans.push_back({12, 12, 12, 3, 0, 1,
                                     IncrementalPDBGoesToGlobals |
                                         IncrementalPDBGoesToModule,
                                     IncrementalPDBUsesGlobalProcRef, 2,
                                     IncrementalPDBScopeAction::Open});
  moduleEntry.typeRefs.push_back(
      {codeview::TiRefKind::IndexRef, 24, 1});
  moduleEntry.stringFixups.push_back({9, 28});
  cache.moduleEntries.push_back(moduleEntry);

  SmallString<128> path = getPath("cache.llpdbcache");
  expectNoError(writeIncrementalPDBCache(path, cache));

  Expected<IncrementalPDBCacheFile> loaded = loadIncrementalPDBCache(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());
  ASSERT_EQ(loaded->typeEntries.size(), 1u);
  ASSERT_EQ(loaded->moduleEntries.size(), 1u);

  const IncrementalPDBTypeCacheEntry &loadedType = loaded->typeEntries.front();
  EXPECT_EQ(loadedType.path, typeEntry.path);
  EXPECT_EQ(loadedType.parentPath, typeEntry.parentPath);
  EXPECT_EQ(loadedType.archiveOffset, typeEntry.archiveOffset);
  EXPECT_EQ(loadedType.kind, typeEntry.kind);
  EXPECT_EQ(loadedType.contentHash, typeEntry.contentHash);
  EXPECT_EQ(loadedType.dependencyHash, typeEntry.dependencyHash);
  EXPECT_EQ(loadedType.endPrecompIdx, typeEntry.endPrecompIdx);
  ASSERT_EQ(loadedType.ghashes.size(), 1u);
  ASSERT_EQ(loadedType.auxGHashes.size(), 1u);
  EXPECT_EQ(0, memcmp(&loadedType.ghashes[0], &typeEntry.ghashes[0],
                      sizeof(codeview::GloballyHashedType)));
  EXPECT_EQ(0, memcmp(&loadedType.auxGHashes[0], &typeEntry.auxGHashes[0],
                      sizeof(codeview::GloballyHashedType)));
  EXPECT_EQ(loadedType.isItemIndexBits, typeEntry.isItemIndexBits);
  EXPECT_EQ(loadedType.auxIsItemIndexBits, typeEntry.auxIsItemIndexBits);

  const IncrementalPDBModuleCacheEntry &loadedModule =
      loaded->moduleEntries.front();
  EXPECT_EQ(loadedModule.path, moduleEntry.path);
  EXPECT_EQ(loadedModule.parentPath, moduleEntry.parentPath);
  EXPECT_EQ(loadedModule.archiveOffset, moduleEntry.archiveOffset);
  EXPECT_EQ(loadedModule.debugSHash, moduleEntry.debugSHash);
  EXPECT_EQ(loadedModule.debugFHash, moduleEntry.debugFHash);
  EXPECT_EQ(loadedModule.relocHash, moduleEntry.relocHash);
  EXPECT_EQ(loadedModule.moduleStreamSize, moduleEntry.moduleStreamSize);
  ASSERT_EQ(loadedModule.chunkPlans.size(), 1u);
  EXPECT_EQ(loadedModule.chunkPlans[0].chunkOrdinal,
            moduleEntry.chunkPlans[0].chunkOrdinal);
  ASSERT_EQ(loadedModule.subsectionPlans.size(), 2u);
  EXPECT_EQ(loadedModule.subsectionPlans[1].kind,
            codeview::DebugSubsectionKind::Symbols);
  ASSERT_EQ(loadedModule.symbolPlans.size(), 1u);
  EXPECT_EQ(loadedModule.symbolPlans[0].rewriteKind,
            moduleEntry.symbolPlans[0].rewriteKind);
  ASSERT_EQ(loadedModule.typeRefs.size(), 1u);
  EXPECT_EQ(loadedModule.typeRefs[0].offset, moduleEntry.typeRefs[0].offset);
  ASSERT_EQ(loadedModule.stringFixups.size(), 1u);
  EXPECT_EQ(loadedModule.stringFixups[0].symOffsetOfReference,
            moduleEntry.stringFixups[0].symOffsetOfReference);
}

TEST_F(IncrementalPDBCacheTest, RejectsInvalidMagic) {
  SmallString<128> path = getPath("broken.llpdbcache");
  {
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_None);
    ASSERT_FALSE(ec);
    std::string bytes(64, '\0');
    bytes[0] = 'b';
    bytes[1] = 'a';
    bytes[2] = 'd';
    os.write(bytes.data(), bytes.size());
  }

  Expected<IncrementalPDBCacheFile> loaded = loadIncrementalPDBCache(path);
  ASSERT_FALSE(static_cast<bool>(loaded));
  consumeError(loaded.takeError());
}

TEST(IncrementalPDBCacheHelpersTest, UsesLlPdbCacheExtension) {
  Configuration config;
  config.outputFile = "C:/tmp/out.exe";
  SmallString<128> path = getIncrementalPDBCachePath(config);
  EXPECT_EQ(path, "C:/tmp/out.llpdbcache");
}

} // namespace
