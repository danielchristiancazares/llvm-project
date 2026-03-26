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

TEST_F(IncrementalPDBCacheTest, RoundTripPreservesTypeAndModuleReplays) {
  IncrementalPDBCacheSnapshot cache;
  cache.linkerBuildId = 0x1111222233334444ULL;
  cache.hardConfigHash = 0x5555666677778888ULL;
  const codeview::GloballyHashedType expectedPrimaryHash = makeHash(0x11);
  const codeview::GloballyHashedType expectedAuxHash = makeHash(0x22);

  cache.typeReplays.push_back(
      IncrementalPDBTypeReplaySnapshot::make<ReplayTypeServerTpiAndIpi>(
          ReplayTypeServerTpiAndIpi{
              "main.obj",
              "archive.lib",
              42,
              0xAAAABBBBCCCCDDDDULL,
              IncrementalPDBTypeReplayBoundary::make<
                  ReplayTypeRecordsSkippingEndPrecomp>(
                  ReplayTypeRecordsSkippingEndPrecomp{17}),
              {expectedPrimaryHash},
              {0x05},
              {expectedAuxHash},
              {0x01}}));

  CachedModuleReplay moduleEntry;
  moduleEntry.path = "main.obj";
  moduleEntry.parentPath = "archive.lib";
  moduleEntry.archiveOffset = 42;
  moduleEntry.debugSHash = 0x100;
  moduleEntry.debugFHash = 0x200;
  moduleEntry.relocHash = 0x300;
  moduleEntry.moduleStreamSize = 64;

  ReplayDebugSChunk debugSChunk;
  debugSChunk.chunkOrdinal = 1;
  debugSChunk.subsections.push_back(
      IncrementalPDBSubsectionReplay::make<ReplayOpaqueSubsection>(
          ReplayOpaqueSubsection{
              codeview::DebugSubsectionKind::StringTable, {4, 8, 0}}));

  ReplaySymbolSubsection symbolSubsection;
  symbolSubsection.location = {12, 28, 1};
  symbolSubsection.symbols.push_back(CachedSymbolReplay{
      {12, 12, 3},
      12,
      SymbolReplayRouting::make<EmitGlobalAndModuleSymbol>(),
      GlobalSymbolReplay::make<ReplayGlobalProcedureReference>(),
      SymbolRewritePlan::make<ReplayProcIdWithFixedTypeIndex>(),
      SymbolScopeReplay::make<ReplayScopeOpeningSymbol>()});
  symbolSubsection.symbols.push_back(CachedSymbolReplay{
      {24, 16, 4},
      16,
      SymbolReplayRouting::make<EmitModuleOnlySymbol>(),
      GlobalSymbolReplay::make<OmitGlobalReplay>(),
      SymbolRewritePlan::make<ReplaySymbolWithDiscoveredTypeRefs>(
          ReplaySymbolWithDiscoveredTypeRefs{
              {{codeview::TiRefKind::IndexRef, 24, 1}}}),
      SymbolScopeReplay::make<ReplayScopeClosingSymbol>()});
  debugSChunk.subsections.push_back(
      IncrementalPDBSubsectionReplay::make<ReplaySymbolSubsection>(
          std::move(symbolSubsection)));

  moduleEntry.chunks.push_back(
      IncrementalPDBChunkReplay::make<ReplayDebugSChunk>(std::move(debugSChunk)));
  moduleEntry.chunks.push_back(
      IncrementalPDBChunkReplay::make<ReplayDebugFChunk>(ReplayDebugFChunk{2}));
  moduleEntry.stringFixups.push_back({9, 28});
  cache.moduleReplays.push_back(std::move(moduleEntry));

  SmallString<128> path = getPath("cache.llpdbcache");
  expectNoError(writeIncrementalPDBCache(path, cache));

  Expected<IncrementalPDBCacheSnapshot> loaded = loadIncrementalPDBCache(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());
  ASSERT_EQ(loaded->typeReplays.size(), 1u);
  ASSERT_EQ(loaded->moduleReplays.size(), 1u);

  bool checkedType = false;
  loaded->typeReplays.front().match(
      [&](const ReplayObjectTypes &) {
        ADD_FAILURE() << "unexpected object type replay";
      },
      [&](const ReplayPrecompiledHeaderTypes &) {
        ADD_FAILURE() << "unexpected PCH type replay";
      },
      [&](const ReplayUsingPrecompiledHeaderTypes &) {
        ADD_FAILURE() << "unexpected using-PCH type replay";
      },
      [&](const ReplayTypeServerTpiOnly &) {
        ADD_FAILURE() << "unexpected TPI-only type replay";
      },
      [&](const ReplayTypeServerTpiAndIpi &loadedType) {
        checkedType = true;
        EXPECT_EQ(loadedType.path, "main.obj");
        EXPECT_EQ(loadedType.parentPath, "archive.lib");
        EXPECT_EQ(loadedType.archiveOffset, 42u);
        EXPECT_EQ(loadedType.contentHash, 0xAAAABBBBCCCCDDDDULL);
        loadedType.boundary.match(
            [&](const ReplayAllTypeRecords &) {
              ADD_FAILURE() << "unexpected full-replay boundary";
            },
            [&](const ReplayTypeRecordsSkippingEndPrecomp &skip) {
              EXPECT_EQ(skip.ghashIndex, 17u);
            });
        ASSERT_EQ(loadedType.ghashes.size(), 1u);
        ASSERT_EQ(loadedType.auxGHashes.size(), 1u);
        EXPECT_EQ(0, memcmp(&loadedType.ghashes[0], &expectedPrimaryHash,
                            sizeof(codeview::GloballyHashedType)));
        EXPECT_EQ(0, memcmp(&loadedType.auxGHashes[0], &expectedAuxHash,
                            sizeof(codeview::GloballyHashedType)));
        EXPECT_EQ(loadedType.isItemIndexBits, std::vector<uint8_t>({0x05}));
        EXPECT_EQ(loadedType.auxIsItemIndexBits, std::vector<uint8_t>({0x01}));
      });
  EXPECT_TRUE(checkedType);

  const CachedModuleReplay &loadedModule = loaded->moduleReplays.front();
  EXPECT_EQ(loadedModule.path, "main.obj");
  EXPECT_EQ(loadedModule.parentPath, "archive.lib");
  EXPECT_EQ(loadedModule.archiveOffset, 42u);
  EXPECT_EQ(loadedModule.debugSHash, 0x100u);
  EXPECT_EQ(loadedModule.debugFHash, 0x200u);
  EXPECT_EQ(loadedModule.relocHash, 0x300u);
  EXPECT_EQ(loadedModule.moduleStreamSize, 64u);
  ASSERT_EQ(loadedModule.chunks.size(), 2u);
  loadedModule.chunks[0].match(
      [&](const ReplayDebugSChunk &chunk) {
        EXPECT_EQ(chunk.chunkOrdinal, 1u);
        ASSERT_EQ(chunk.subsections.size(), 2u);
        chunk.subsections[0].match(
            [&](const ReplayOpaqueSubsection &subsection) {
              EXPECT_EQ(subsection.kind, codeview::DebugSubsectionKind::StringTable);
              EXPECT_EQ(subsection.location.recordOffset, 4u);
              EXPECT_EQ(subsection.location.recordLength, 8u);
              EXPECT_EQ(subsection.location.relocIndex, 0u);
            },
            [&](const ReplaySymbolSubsection &) {
              ADD_FAILURE() << "unexpected symbol subsection";
            });
        chunk.subsections[1].match(
            [&](const ReplayOpaqueSubsection &) {
              ADD_FAILURE() << "unexpected opaque subsection";
            },
            [&](const ReplaySymbolSubsection &subsection) {
              EXPECT_EQ(subsection.location.recordOffset, 12u);
              EXPECT_EQ(subsection.location.recordLength, 28u);
              EXPECT_EQ(subsection.location.relocIndex, 1u);
              ASSERT_EQ(subsection.symbols.size(), 2u);

              const CachedSymbolReplay &first = subsection.symbols[0];
              EXPECT_EQ(first.location.recordOffset, 12u);
              EXPECT_EQ(first.location.recordLength, 12u);
              EXPECT_EQ(first.location.relocIndex, 3u);
              EXPECT_EQ(first.alignedLength, 12u);
              EXPECT_TRUE(first.routing.match(
                  [&](const EmitGlobalOnlySymbol &) { return false; },
                  [&](const EmitModuleOnlySymbol &) { return false; },
                  [&](const EmitGlobalAndModuleSymbol &) { return true; }));
              EXPECT_TRUE(first.globalReplay.match(
                  [&](const OmitGlobalReplay &) { return false; },
                  [&](const ReplayGlobalSymbolBytes &) { return false; },
                  [&](const ReplayGlobalProcedureReference &) { return true; }));
              EXPECT_TRUE(first.rewrite.match(
                  [&](const ReplaySymbolWithoutTypeRewrite &) { return false; },
                  [&](const ReplayProcIdEndSymbol &) { return false; },
                  [&](const ReplayProcIdWithFixedTypeIndex &) { return true; },
                  [&](const ReplaySymbolWithDiscoveredTypeRefs &) {
                    return false;
                  }));
              EXPECT_TRUE(first.scope.match(
                  [&](const ReplayStandaloneSymbol &) { return false; },
                  [&](const ReplayScopeOpeningSymbol &) { return true; },
                  [&](const ReplayScopeClosingSymbol &) { return false; }));

              const CachedSymbolReplay &second = subsection.symbols[1];
              EXPECT_EQ(second.location.recordOffset, 24u);
              EXPECT_EQ(second.location.recordLength, 16u);
              EXPECT_EQ(second.location.relocIndex, 4u);
              EXPECT_EQ(second.alignedLength, 16u);
              EXPECT_TRUE(second.routing.match(
                  [&](const EmitGlobalOnlySymbol &) { return false; },
                  [&](const EmitModuleOnlySymbol &) { return true; },
                  [&](const EmitGlobalAndModuleSymbol &) { return false; }));
              EXPECT_TRUE(second.globalReplay.match(
                  [&](const OmitGlobalReplay &) { return true; },
                  [&](const ReplayGlobalSymbolBytes &) { return false; },
                  [&](const ReplayGlobalProcedureReference &) { return false; }));
              second.rewrite.match(
                  [&](const ReplaySymbolWithoutTypeRewrite &) {
                    ADD_FAILURE() << "unexpected no-type rewrite";
                  },
                  [&](const ReplayProcIdEndSymbol &) {
                    ADD_FAILURE() << "unexpected proc-id-end rewrite";
                  },
                  [&](const ReplayProcIdWithFixedTypeIndex &) {
                    ADD_FAILURE() << "unexpected fixed-index rewrite";
                  },
                  [&](const ReplaySymbolWithDiscoveredTypeRefs &generic) {
                    ASSERT_EQ(generic.typeRefs.size(), 1u);
                    EXPECT_EQ(generic.typeRefs[0].kind,
                              codeview::TiRefKind::IndexRef);
                    EXPECT_EQ(generic.typeRefs[0].offset, 24u);
                    EXPECT_EQ(generic.typeRefs[0].count, 1u);
                  });
              EXPECT_TRUE(second.scope.match(
                  [&](const ReplayStandaloneSymbol &) { return false; },
                  [&](const ReplayScopeOpeningSymbol &) { return false; },
                  [&](const ReplayScopeClosingSymbol &) { return true; }));
            });
      },
      [&](const ReplayDebugFChunk &) { ADD_FAILURE() << "unexpected debug F"; });
  loadedModule.chunks[1].match(
      [&](const ReplayDebugSChunk &) { ADD_FAILURE() << "unexpected debug S"; },
      [&](const ReplayDebugFChunk &chunk) { EXPECT_EQ(chunk.chunkOrdinal, 2u); });
  ASSERT_EQ(loadedModule.stringFixups.size(), 1u);
  EXPECT_EQ(loadedModule.stringFixups[0].strTabOffset, 9u);
  EXPECT_EQ(loadedModule.stringFixups[0].symOffsetOfReference, 28u);
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

  Expected<IncrementalPDBCacheSnapshot> loaded = loadIncrementalPDBCache(path);
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
