#include "../../COFF/IncrementalState.h"
#include "../../COFF/Incremental.h"
#include "../../COFF/Chunks.h"
#include "../../COFF/Symbols.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <climits>
#include <cstring>
#include <optional>

using namespace lld::coff;
using namespace llvm;

namespace {

class IncrementalStateTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_FALSE(sys::fs::createUniqueDirectory("lld-incremental-state", testDir));
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

static std::optional<size_t>
getSelectedFreeSlotIndex(const IncrementalFreeSlotSelection &selection) {
  return selection.match(
      [](const NoFreeSlotFit &) -> std::optional<size_t> { return std::nullopt; },
      [](const SelectedFreeSlot &selected) -> std::optional<size_t> {
        return selected.index;
      });
}

static std::optional<uint64_t>
getTailReserveStartRVA(const IncrementalTailReserveSelection &selection) {
  return selection.match(
      [](const TailReserveUnavailable &) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      [](const TailReserveStart &start) -> std::optional<uint64_t> {
        return start.rva;
      });
}

static std::optional<uint64_t>
getSelectedPoolThunkRVA(const IncrementalTextThunkSelection &selection) {
  return selection.match(
      [](const PoolThunkUnavailable &) -> std::optional<uint64_t> {
        return std::nullopt;
      },
      [](const SelectedPoolThunkRVA &selected)
          -> std::optional<uint64_t> { return selected.rva; });
}

TEST_F(IncrementalStateTest, RoundTripPreservesExtendedFields) {
  IncrementalStateFile state;
  state.layoutMode = IncrementalLayoutMode::Slotted;
  state.machine = AMD64;
  state.outputHash = 0x1111;
  state.outputSize = 0x2222;
  state.hardConfigHash = 0x3333;
  state.softConfigHash = 0x4444;
  state.importTopologyHash = 0x5555;
  state.exportTopologyHash = 0x6666;
  state.resourceInputHash = 0x7777;
  state.sizeOfHeaders = 0x8888;
  state.sizeOfImage = 0x9999;
  state.outputPath = "out.exe";

  IncrementalInputState input;
  input.name = "main.obj";
  input.parentName = "libfoo.lib";
  input.archiveOffset = 42;
  input.contentHash = 0xaaaa;
  input.size = 123;
  state.inputs.push_back(input);

  IncrementalSectionState section;
  section.name = ".text";
  section.characteristics = 0x60000020;
  section.rva = 0x1000;
  section.fileOffset = 0x400;
  section.virtualSize = 0x200;
  section.rawSize = 0x200;
  section.firstChunk = 0;
  section.chunkCount = 1;
  state.sections.push_back(section);

  IncrementalChunkState chunk;
  chunk.kind = IncrementalChunkKind::ObjSection;
  chunk.key = "obj:0:comdat:main";
  chunk.sectionIndex = 0;
  chunk.inputIndex = 0;
  chunk.outputCharacteristics = 0x60000020;
  chunk.sectionNumber = 1;
  chunk.alignment = 16;
  chunk.rva = 0x1000;
  chunk.size = 32;
  chunk.slotCapacity = 48;
  chunk.contentHash = 0xbbbb;
  chunk.symbolHash = 0xcccc;
  state.chunks.push_back(chunk);

  IncrementalSymbolState symbol;
  symbol.name = "main";
  symbol.auxiliaryKey = "obj:0:comdat:main";
  symbol.kind = IncrementalSymbolKind::Regular;
  symbol.inputIndex = 0;
  symbol.value = 0;
  state.symbols.push_back(symbol);

  IncrementalSectionEnvelopeState envelope;
  envelope.name = ".text";
  envelope.characteristics = 0x60000020;
  envelope.sectionRVA = 0x1000;
  envelope.maxSectionEndRVA = 0x2000;
  envelope.activeEndRVA = 0x1200;
  envelope.layoutKind = IncrementalSectionLayoutKind::TextFreeSlots;
  state.sectionEnvelopes.push_back(envelope);

  IncrementalSlotRecordState slot;
  slot.envelopeIndex = 0;
  slot.startRVA = 0x1000;
  slot.capacity = 48;
  slot.committedSize = 32;
  slot.minAlignment = 16;
  slot.fillByte = 0xCC;
  slot.state = IncrementalSlotState::Occupied;
  slot.occupantKey = "obj:0:comdat:main";
  state.slotRecords.push_back(slot);

  IncrementalPackedSectionState packedSection;
  packedSection.envelopeIndex = 0;
  packedSection.activePrefixSize = 32;
  packedSection.reserveSize = 16;
  packedSection.recordKeys.push_back("obj:0:comdat:main");
  state.packedSections.push_back(packedSection);

  IncrementalPlacementState placement;
  placement.key = "obj:0:comdat:main";
  placement.envelopeIndex = 0;
  placement.kind = IncrementalPlacementKind::ExistingSlot;
  placement.startRVA = 0x1000;
  placement.size = 32;
  placement.alignment = 16;
  state.placements.push_back(placement);

  IncrementalTextRedirectState redirect;
  redirect.targetKey = "obj:0:comdat:main";
  redirect.canonicalSymbol = "main";
  redirect.redirectRVA = 0x1000;
  redirect.redirectCapacity = 16;
  redirect.bodyRVA = 0x1200;
  redirect.poolThunkRVA = 0;
  state.textRedirects.push_back(redirect);

  state.textThunkPool.poolStartRVA = 0x1800;
  state.textThunkPool.poolEndRVA = 0x1A00;
  state.textThunkPool.nextFreeRVA = 0x1A00;

  SmallString<128> path = getPath("state.llilk");
  expectNoError(writeIncrementalState(path, state));

  Expected<IncrementalStateFile> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());

  EXPECT_EQ(loaded->version, 6u);
  EXPECT_EQ(loaded->layoutMode, IncrementalLayoutMode::Slotted);
  EXPECT_EQ(loaded->machine, AMD64);
  EXPECT_EQ(loaded->importTopologyHash, state.importTopologyHash);
  EXPECT_EQ(loaded->exportTopologyHash, state.exportTopologyHash);
  EXPECT_EQ(loaded->resourceInputHash, state.resourceInputHash);
  ASSERT_EQ(loaded->inputs.size(), 1u);
  EXPECT_EQ(loaded->inputs[0].parentName, "libfoo.lib");
  EXPECT_EQ(loaded->inputs[0].archiveOffset, 42u);
  ASSERT_EQ(loaded->symbols.size(), 1u);
  EXPECT_EQ(loaded->symbols[0].name, "main");
  EXPECT_EQ(loaded->symbols[0].auxiliaryKey, "obj:0:comdat:main");
  EXPECT_EQ(loaded->symbols[0].kind, IncrementalSymbolKind::Regular);
  ASSERT_EQ(loaded->sectionEnvelopes.size(), 1u);
  EXPECT_EQ(loaded->sectionEnvelopes[0].name, ".text");
  EXPECT_EQ(loaded->sectionEnvelopes[0].layoutKind,
            IncrementalSectionLayoutKind::TextFreeSlots);
  ASSERT_EQ(loaded->slotRecords.size(), 1u);
  EXPECT_EQ(loaded->slotRecords[0].occupantKey, "obj:0:comdat:main");
  EXPECT_EQ(loaded->slotRecords[0].fillByte, 0xCC);
  EXPECT_EQ(loaded->slotRecords[0].state, IncrementalSlotState::Occupied);
  ASSERT_EQ(loaded->packedSections.size(), 1u);
  ASSERT_EQ(loaded->packedSections[0].recordKeys.size(), 1u);
  EXPECT_EQ(loaded->packedSections[0].recordKeys[0], "obj:0:comdat:main");
  ASSERT_EQ(loaded->placements.size(), 1u);
  EXPECT_EQ(loaded->placements[0].key, "obj:0:comdat:main");
  EXPECT_EQ(loaded->placements[0].kind,
            IncrementalPlacementKind::ExistingSlot);
  ASSERT_EQ(loaded->textRedirects.size(), 1u);
  EXPECT_EQ(loaded->textRedirects[0].canonicalSymbol, "main");
  EXPECT_EQ(loaded->textRedirects[0].redirectCapacity, 16u);
  EXPECT_EQ(loaded->textThunkPool.poolStartRVA, 0x1800u);
  EXPECT_EQ(loaded->textThunkPool.poolEndRVA, 0x1A00u);
  EXPECT_EQ(loaded->textThunkPool.nextFreeRVA, 0x1A00u);
}

TEST_F(IncrementalStateTest, RejectsInvalidMagicAndVersion) {
  SmallString<128> path = getPath("broken.llilk");
  {
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_None);
    ASSERT_FALSE(ec);
    std::string bytes(128, '\0');
    bytes[0] = 'b';
    bytes[1] = 'a';
    bytes[2] = 'd';
    os.write(bytes.data(), bytes.size());
  }

  Expected<IncrementalStateFile> badMagic = loadIncrementalState(path);
  ASSERT_FALSE(static_cast<bool>(badMagic));
  consumeError(badMagic.takeError());

  IncrementalStateFile state;
  state.machine = AMD64;
  state.outputPath = "out.exe";
  expectNoError(writeIncrementalState(path, state));

  auto bufferOrErr =
      MemoryBuffer::getFile(path, /*IsText=*/false, /*RequiresNullTerminator=*/false);
  ASSERT_TRUE(static_cast<bool>(bufferOrErr));
  std::string bytes = (*bufferOrErr)->getBuffer().str();
  ASSERT_GE(bytes.size(), 12u);
  bytes[8] = static_cast<char>(0x7f);
  bytes[9] = 0;
  bytes[10] = 0;
  bytes[11] = 0;
  {
    std::error_code ec;
    raw_fd_ostream os(path, ec, sys::fs::OF_None);
    ASSERT_FALSE(ec);
    os.write(bytes.data(), bytes.size());
  }

  Expected<IncrementalStateFile> badVersion = loadIncrementalState(path);
  ASSERT_FALSE(static_cast<bool>(badVersion));
  consumeError(badVersion.takeError());
}

TEST(IncrementalHelpersTest, BestFitSelectionHonorsCapacityAndAlignment) {
  std::vector<IncrementalSlotRecordState> slots(3);
  slots[0].startRVA = 0x1000;
  slots[0].capacity = 32;
  slots[1].startRVA = 0x1010;
  slots[1].capacity = 24;
  slots[2].startRVA = 0x1024;
  slots[2].capacity = 24;

  std::optional<size_t> slot =
      getSelectedFreeSlotIndex(findBestFitIncrementalFreeSlot(slots, 16, 16));
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 1u);

  slot = getSelectedFreeSlotIndex(findBestFitIncrementalFreeSlot(slots, 16, 32));
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 0u);

  EXPECT_FALSE(
      getSelectedFreeSlotIndex(findBestFitIncrementalFreeSlot(slots, 64, 16))
          .has_value());
}

TEST(IncrementalHelpersTest, TailReserveAllocationAlignsAndRejectsOverflow) {
  std::optional<uint64_t> start = getTailReserveStartRVA(
      allocateIncrementalTailReserve(0x1003, 0x1010, 4, 4));
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(*start, 0x1004u);

  EXPECT_FALSE(getTailReserveStartRVA(
                   allocateIncrementalTailReserve(0x100f, 0x1010, 4, 4))
                   .has_value());
}

TEST(IncrementalHelpersTest, Amd64Rel32RangeHelperChecksBoundaries) {
  uint64_t source = 0x1000;
  uint64_t maxInRange = source + 4 + uint64_t(INT32_MAX);
  EXPECT_TRUE(
      isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32, source,
                                     maxInRange));
  EXPECT_FALSE(isIncrementalAmd64Rel32InRange(
      llvm::COFF::IMAGE_REL_AMD64_REL32, source, maxInRange + 1));

  uint64_t minInRange = source + 4 - uint64_t(0x80000000ULL);
  EXPECT_TRUE(
      isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32, source,
                                     minInRange));
  EXPECT_FALSE(isIncrementalAmd64Rel32InRange(
      llvm::COFF::IMAGE_REL_AMD64_REL32, source, minInRange - 1));
}

TEST(IncrementalHelpersTest, EntryRedirectChunkRangeChecks) {
  EmptyChunk bodyChunk;
  bodyChunk.setRVA(0x2000);
  DefinedSynthetic body("body", &bodyChunk);

  IncrementalEntryRedirectChunkX64 redirect("redir", &body, 16, 16);
  redirect.setRVA(0x1000);
  EXPECT_TRUE(redirect.verifyRanges());

  bodyChunk.setRVA(0x90000000ULL);
  EXPECT_FALSE(redirect.verifyRanges());
}

TEST(IncrementalHelpersTest, LongThunkChunkAddsDir64BaseReloc) {
  EmptyChunk bodyChunk;
  bodyChunk.setRVA(0x3000);
  DefinedSynthetic body("body", &bodyChunk);

  IncrementalLongThunkChunkX64 thunk("pool", &body, 0x140000000ULL);
  thunk.setRVA(0x1800);

  std::vector<Baserel> relocs;
  thunk.getBaserels(&relocs);
  ASSERT_EQ(relocs.size(), 1u);
  EXPECT_EQ(relocs[0].rva, 0x1802u);
  EXPECT_EQ(relocs[0].type, llvm::COFF::IMAGE_REL_BASED_DIR64);
}

TEST(IncrementalHelpersTest, LongThunkChunkWritesImageBaseAdjustedVA) {
  EmptyChunk bodyChunk;
  bodyChunk.setRVA(0x3000);
  DefinedSynthetic body("body", &bodyChunk);

  IncrementalLongThunkChunkX64 thunk("pool", &body, 0x140000000ULL);
  uint8_t buf[16] = {};
  thunk.writeTo(buf);

  uint64_t immediate = 0;
  memcpy(&immediate, buf + 2, sizeof(immediate));
  EXPECT_EQ(buf[0], 0x48);
  EXPECT_EQ(buf[1], 0xB8);
  EXPECT_EQ(immediate, 0x140003000ULL);
  EXPECT_EQ(buf[10], 0xFF);
  EXPECT_EQ(buf[11], 0xE0);
}

TEST(IncrementalHelpersTest, PersistedSlotChunkFilterDropsTextLongThunks) {
  EmptyChunk bodyChunk;
  DefinedSynthetic body("body", &bodyChunk);
  IncrementalLongThunkChunkX64 thunk("pool", &body, 0x140000000ULL);
  IncrementalPaddingChunk padding(".text", 0x60000020, 16, 0xCC);

  EXPECT_FALSE(isIncrementalPersistedSlotChunk(
      IncrementalSectionLayoutKind::TextFreeSlots, thunk));
  EXPECT_TRUE(isIncrementalPersistedSlotChunk(
      IncrementalSectionLayoutKind::TextFreeSlots, padding));
  EXPECT_FALSE(
      isIncrementalPersistedSlotChunk(
          IncrementalSectionLayoutKind::TextFreeSlots, bodyChunk));
}

TEST(IncrementalHelpersTest, ChooseTextThunkRVAReusesValidExistingSlot) {
  std::optional<uint64_t> thunkRVA = getSelectedPoolThunkRVA(
      chooseIncrementalTextThunkRVA(0x2ff0, 0x2400, 0x2fe0, 0x3000, {}));
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x2ff0u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAAllocatesFreshSlotWhenExistingOneIsInvalid) {
  std::optional<uint64_t> thunkRVA = getSelectedPoolThunkRVA(
      chooseIncrementalTextThunkRVA(0x2fe0, 0x2ff8, 0x3010, 0x3020, {}));
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x3000u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAReusesFreedSlotBeforeScanningBelowPoolCursor) {
  uint64_t freedThunkRVAs[] = {0x2fd0, 0x2ff0};
  std::optional<uint64_t> thunkRVA = getSelectedPoolThunkRVA(
      chooseIncrementalTextThunkRVA(0, 0x2fc0, 0x2fd0, 0x3000, {},
                                    freedThunkRVAs));
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x2ff0u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAIgnoresClaimedOrInvalidFreedSlots) {
  uint64_t claimedThunkRVAs[] = {0x2ff0};
  uint64_t freedThunkRVAs[] = {0x2fb0, 0x2ff0, 0x2fd0};
  std::optional<uint64_t> thunkRVA = getSelectedPoolThunkRVA(
      chooseIncrementalTextThunkRVA(0, 0x2fc0, 0x2fd0, 0x3000,
                                    claimedThunkRVAs, freedThunkRVAs));
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x2fd0u);
}

TEST(IncrementalHelpersTest,
     PlanTextThunkAssignmentsReusesSlotFreedEarlierInPass) {
  std::vector<IncrementalTextThunkPlanState> plans(2);
  plans[0].redirectRVA = 0x1200;
  plans[0].bodyRVA = 0x1210;
  plans[0].poolThunkRVA = 0x2ff0;
  plans[0].engagement = IncrementalRedirectEngagement::Installed;
  plans[0].provenance = IncrementalRedirectProvenance::LegacyRedirect;
  plans[0].targeting = IncrementalRedirectTargeting::PoolThunkTarget;

  plans[1].redirectRVA = 0x1300;
  plans[1].bodyRVA = 0x90000000ULL;
  plans[1].engagement = IncrementalRedirectEngagement::Installed;

  uint64_t poolCursor = 0x2fd0;
  uint64_t poolStart = 0;
  planIncrementalTextThunkAssignments(plans, 0x2fc0, poolCursor, poolStart,
                                      0x3000, true);

  EXPECT_EQ(plans[0].engagement, IncrementalRedirectEngagement::Installed);
  EXPECT_EQ(plans[0].targeting,
            IncrementalRedirectTargeting::DirectBodyTarget);
  EXPECT_EQ(plans[0].poolThunkRVA, 0u);

  EXPECT_EQ(plans[1].engagement, IncrementalRedirectEngagement::Installed);
  EXPECT_EQ(plans[1].targeting,
            IncrementalRedirectTargeting::PoolThunkTarget);
  EXPECT_EQ(plans[1].poolThunkRVA, 0x2ff0u);
  EXPECT_EQ(poolCursor, 0x2fd0u);
  EXPECT_EQ(poolStart, 0x2ff0u);
}

TEST(IncrementalHelpersTest, ChooseTextThunkRVAFailsWhenPoolIsExhausted) {
  EXPECT_FALSE(getSelectedPoolThunkRVA(
                   chooseIncrementalTextThunkRVA(0x2ff0, 0x2ff8, 0x3000, 0x3000,
                                                 {}))
                   .has_value());
}

TEST(IncrementalHelpersTest, RedirectStateRoundTripPreservesPoolState) {
  IncrementalStateFile state;
  state.layoutMode = IncrementalLayoutMode::Slotted;
  state.outputPath = "out.exe";
  state.textThunkPool.poolStartRVA = 0x4000;
  state.textThunkPool.poolEndRVA = 0x5000;
  state.textThunkPool.nextFreeRVA = 0x4FF0;

  IncrementalTextRedirectState redirect;
  redirect.targetKey = "obj:0:comdat:target";
  redirect.canonicalSymbol = "target";
  redirect.redirectRVA = 0x1200;
  redirect.redirectCapacity = 16;
  redirect.bodyRVA = 0x2400;
  redirect.poolThunkRVA = 0x4FF0;
  state.textRedirects.push_back(redirect);

  SmallString<128> path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("redirect-pool", "llilk", path));
  Error err = writeIncrementalState(path, state);
  ASSERT_FALSE(static_cast<bool>(err)) << toString(std::move(err));
  Expected<IncrementalStateFile> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());
  ASSERT_EQ(loaded->textRedirects.size(), 1u);
  EXPECT_EQ(loaded->textRedirects[0].poolThunkRVA, 0x4FF0u);
  EXPECT_EQ(loaded->textThunkPool.poolStartRVA, 0x4000u);
  EXPECT_EQ(loaded->textThunkPool.poolEndRVA, 0x5000u);
  EXPECT_EQ(loaded->textThunkPool.nextFreeRVA, 0x4FF0u);
  std::error_code ec = sys::fs::remove(path);
  EXPECT_FALSE(ec);
}

} // namespace
