#include "../../COFF/Chunks.h"
#include "../../COFF/Incremental.h"
#include "../../COFF/IncrementalState.h"
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
    ASSERT_FALSE(
        sys::fs::createUniqueDirectory("lld-incremental-state", testDir));
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
getSelectedFreeSlotIndex(ArrayRef<IncrementalPreservedSlot> slots,
                         uint64_t size, uint32_t alignment) {
  std::optional<size_t> selectedSlot;
  matchBestFitIncrementalFreeSlot(
      slots, size, alignment,
      [&](size_t slotIndex) { selectedSlot = slotIndex; }, [&]() {});
  return selectedSlot;
}

static std::optional<uint64_t> getTailReserveStartRVA(uint64_t tailCursor,
                                                      uint64_t maxSectionEndRVA,
                                                      uint64_t size,
                                                      uint32_t alignment) {
  std::optional<uint64_t> tailReserveStartRVA;
  matchIncrementalTailReserve(
      tailCursor, maxSectionEndRVA, size, alignment,
      [&](uint64_t startRVA) { tailReserveStartRVA = startRVA; }, [&]() {});
  return tailReserveStartRVA;
}

static std::optional<uint64_t>
getSelectedPoolThunkRVA(uint64_t oldPoolThunkRVA, uint64_t tailCursor,
                        uint64_t poolCursor, uint64_t poolEndRVA,
                        ArrayRef<uint64_t> claimedThunkRVAs,
                        ArrayRef<uint64_t> freedThunkRVAs = {}) {
  std::optional<uint64_t> selectedPoolThunkRVA;
  matchIncrementalTextThunkRVA(
      oldPoolThunkRVA, tailCursor, poolCursor, poolEndRVA, claimedThunkRVAs,
      [&](uint64_t thunkRVA) { selectedPoolThunkRVA = thunkRVA; }, [&]() {},
      freedThunkRVAs);
  return selectedPoolThunkRVA;
}

static const TextSlotSectionSnapshot *
findTextSlotSectionSnapshot(const IncrementalSectionSnapshot &section) {
  const TextSlotSectionSnapshot *textSection = nullptr;
  matchIncrementalTextSlotSectionSnapshot(
      section,
      [&](const TextSlotSectionSnapshot &matchedSection) {
        textSection = &matchedSection;
      },
      [&]() {});
  return textSection;
}

static const IncrementalPackedPrefixSectionSnapshot *
findPackedPrefixSectionSnapshot(const IncrementalSectionSnapshot &section) {
  const IncrementalPackedPrefixSectionSnapshot *packedSection = nullptr;
  matchIncrementalPackedPrefixSectionSnapshot(
      section,
      [&](const IncrementalPackedPrefixSectionSnapshot &matchedSection) {
        packedSection = &matchedSection;
      },
      [&]() {});
  return packedSection;
}

static std::optional<std::string>
getPreservedSlotOccupant(const IncrementalPreservedSlot &slot) {
  std::optional<std::string> occupant;
  matchIncrementalPreservedSlotOccupancy(
      slot, [&](const FreeSlotRecord &) {},
      [&](const OccupiedSlotRecord &occupied) {
        occupant = occupied.occupantKey;
      });
  return occupant;
}

static std::string
describeResolvedSymbolForTest(const IncrementalResolvedSymbolSnapshot &symbol) {
  SmallString<128> buffer;
  raw_svector_ostream os(buffer);
  symbol.match(
      [&](const ObjFileRegularResolvedSymbol &regular) {
        os << "objreg:" << regular.name << ':' << regular.inputIndex << ':'
           << regular.value << ':' << regular.chunkKey;
      },
      [&](const BitcodeRegularResolvedSymbol &regular) {
        os << "bcreg:" << regular.name << ':' << regular.value;
      },
      [&](const ObjFileCommonResolvedSymbol &common) {
        os << "objcommon:" << common.name << ':' << common.inputIndex << ':'
           << common.size << ':' << common.alignment;
      },
      [&](const BitcodeCommonResolvedSymbol &common) {
        os << "bccommon:" << common.name << ':' << common.size << ':'
           << common.alignment;
      },
      [&](const ImportDataResolvedSymbol &importData) {
        os << "importdata:" << importData.name << ':' << importData.ordinal
           << ':' << importData.dllName << ':' << importData.externalName
           << ':' << importData.typeInfo;
      },
      [&](const ImportThunkResolvedSymbol &importThunk) {
        os << "importthunk:" << importThunk.name << ':'
           << importThunk.wrappedSymbolName;
      },
      [&](const LocalImportResolvedSymbol &localImport) {
        os << "localimport:" << localImport.name << ':' << localImport.chunkKey;
      },
      [&](const AbsoluteResolvedSymbol &absolute) {
        os << "absolute:" << absolute.name << ':' << absolute.value;
      },
      [&](const ChunkBackedSyntheticResolvedSymbol &synthetic) {
        os << "chunksynth:" << synthetic.name << ':' << synthetic.chunkKey;
      },
      [&](const ImageBaseSyntheticResolvedSymbol &synthetic) {
        os << "imagebase:" << synthetic.name;
      });
  return std::string(buffer);
}

TEST_F(IncrementalStateTest, RoundTripPreservesExtendedFields) {
  IncrementalBaselineSnapshot state;
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
  TextSlotSectionSnapshot textSection;
  textSection.slotSection.section = section;
  textSection.slotSection.maxSectionEndRVA = 0x2000;
  textSection.slotSection.activeEndRVA = 0x1200;
  textSection.slotSection.slots.push_back(
      IncrementalPreservedSlot::make<OccupiedSlotRecord>(OccupiedSlotRecord{
          IncrementalPreservedSlotState{0x1000, 48, 32, 16, 0xCC},
          "obj:0:comdat:main"}));
  textSection.slotSection.slots.push_back(
      IncrementalPreservedSlot::make<FreeSlotRecord>(
          FreeSlotRecord{IncrementalPreservedSlotState{0x1030, 16, 0, 16,
                                                       0xCC}}));
  textSection.slotSection.preservedChunks.push_back(
      ExistingSlotChunkPlacement{"obj:0:comdat:main", 0x1000, 32, 16});

  IncrementalTextRedirectState redirect;
  redirect.targetKey = "obj:0:comdat:main";
  redirect.canonicalSymbol = "main";
  redirect.redirectRVA = 0x1000;
  redirect.redirectCapacity = 16;
  redirect.bodyRVA = 0x1200;
  redirect.poolThunkRVA = 0;
  textSection.redirects.push_back(redirect);
  textSection.thunkPool.poolStartRVA = 0x1800;
  textSection.thunkPool.poolEndRVA = 0x1A00;
  textSection.thunkPool.nextFreeRVA = 0x1A00;
  state.sections.push_back(
      IncrementalSectionSnapshot::make<TextSlotSectionSnapshot>(
          std::move(textSection)));

  IncrementalSectionState packedSectionState;
  packedSectionState.name = ".pdata";
  packedSectionState.characteristics = 0x40000040;
  packedSectionState.rva = 0x2000;
  packedSectionState.fileOffset = 0x600;
  packedSectionState.virtualSize = 0x40;
  packedSectionState.rawSize = 0x40;
  packedSectionState.firstChunk = 1;
  packedSectionState.chunkCount = 0;

  PDataPackedPrefixSectionSnapshot packedSection;
  packedSection.packedSection.section = packedSectionState;
  packedSection.packedSection.activePrefixSize = 32;
  packedSection.packedSection.reserveSize = 16;
  packedSection.packedSection.members.push_back(
      PackedPrefixChunkPlacement{"obj:0:comdat:main", 0x2000, 32, 16});
  state.sections.push_back(
      IncrementalSectionSnapshot::make<PDataPackedPrefixSectionSnapshot>(
          std::move(packedSection)));

  state.chunks.push_back(
      IncrementalChunkSnapshot::make<ObjSectionChunkSnapshot>(
          ObjSectionChunkSnapshot{IncrementalChunkState{"obj:0:comdat:main", 0,
                                                        0x60000020, 16, 0x1000,
                                                        32, 48},
                                  0, 1, 0xbbbb, 0xcccc}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ObjFileRegularResolvedSymbol>(
          ObjFileRegularResolvedSymbol{"main", 0, 0, "obj:0:comdat:main"}));

  SmallString<128> path = getPath("state.llilk");
  expectNoError(writeIncrementalState(path, state));

  Expected<IncrementalBaselineSnapshot> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());

  EXPECT_EQ(loaded->machine, AMD64);
  EXPECT_EQ(loaded->importTopologyHash, state.importTopologyHash);
  EXPECT_EQ(loaded->exportTopologyHash, state.exportTopologyHash);
  EXPECT_EQ(loaded->resourceInputHash, state.resourceInputHash);
  ASSERT_EQ(loaded->inputs.size(), 1u);
  EXPECT_EQ(loaded->inputs[0].parentName, "libfoo.lib");
  EXPECT_EQ(loaded->inputs[0].archiveOffset, 42u);
  ASSERT_EQ(loaded->chunks.size(), 1u);
  loaded->chunks[0].match(
      [&](const ObjSectionChunkSnapshot &chunk) {
        EXPECT_EQ(chunk.chunk.key, "obj:0:comdat:main");
        EXPECT_EQ(chunk.inputIndex, 0u);
        EXPECT_EQ(chunk.sectionNumber, 1u);
        EXPECT_EQ(chunk.contentHash, 0xbbbbu);
        EXPECT_EQ(chunk.symbolHash, 0xccccu);
      },
      [&](const SyntheticChunkSnapshot &) {
        ADD_FAILURE() << "unexpected synthetic chunk";
      },
      [&](const PaddingChunkSnapshot &) {
        ADD_FAILURE() << "unexpected padding chunk";
      },
      [&](const EntryRedirectChunkSnapshot &) {
        ADD_FAILURE() << "unexpected redirect chunk";
      },
      [&](const LongThunkChunkSnapshot &) {
        ADD_FAILURE() << "unexpected thunk chunk";
      });
  ASSERT_EQ(loaded->symbols.size(), 1u);
  loaded->symbols[0].match(
      [&](const ObjFileRegularResolvedSymbol &symbol) {
        EXPECT_EQ(symbol.name, "main");
        EXPECT_EQ(symbol.inputIndex, 0u);
        EXPECT_EQ(symbol.chunkKey, "obj:0:comdat:main");
      },
      [&](const BitcodeRegularResolvedSymbol &) {
        ADD_FAILURE() << "unexpected bitcode regular symbol";
      },
      [&](const ObjFileCommonResolvedSymbol &) {
        ADD_FAILURE() << "unexpected common symbol";
      },
      [&](const BitcodeCommonResolvedSymbol &) {
        ADD_FAILURE() << "unexpected bitcode common symbol";
      },
      [&](const ImportDataResolvedSymbol &) {
        ADD_FAILURE() << "unexpected import-data symbol";
      },
      [&](const ImportThunkResolvedSymbol &) {
        ADD_FAILURE() << "unexpected import-thunk symbol";
      },
      [&](const LocalImportResolvedSymbol &) {
        ADD_FAILURE() << "unexpected local import symbol";
      },
      [&](const AbsoluteResolvedSymbol &) {
        ADD_FAILURE() << "unexpected absolute symbol";
      },
      [&](const ChunkBackedSyntheticResolvedSymbol &) {
        ADD_FAILURE() << "unexpected synthetic symbol";
      },
      [&](const ImageBaseSyntheticResolvedSymbol &) {
        ADD_FAILURE() << "unexpected __ImageBase synthetic symbol";
      });
  ASSERT_EQ(loaded->sections.size(), 2u);
  const TextSlotSectionSnapshot *loadedText =
      findTextSlotSectionSnapshot(loaded->sections[0]);
  ASSERT_NE(loadedText, nullptr);
  EXPECT_EQ(loadedText->slotSection.section.name, ".text");
  EXPECT_EQ(loadedText->slotSection.maxSectionEndRVA, 0x2000u);
  EXPECT_EQ(loadedText->slotSection.activeEndRVA, 0x1200u);
  ASSERT_EQ(loadedText->slotSection.slots.size(), 2u);
  std::optional<std::string> occupant =
      getPreservedSlotOccupant(loadedText->slotSection.slots[0]);
  ASSERT_TRUE(occupant.has_value());
  EXPECT_EQ(*occupant, "obj:0:comdat:main");
  EXPECT_EQ(getIncrementalPreservedSlotState(loadedText->slotSection.slots[0])
                .fillByte,
            0xCC);
  EXPECT_FALSE(
      getPreservedSlotOccupant(loadedText->slotSection.slots[1]).has_value());
  EXPECT_EQ(getIncrementalPreservedSlotState(loadedText->slotSection.slots[1])
                .committedSize,
            0u);
  ASSERT_EQ(loadedText->slotSection.preservedChunks.size(), 1u);
  EXPECT_EQ(loadedText->slotSection.preservedChunks[0].key,
            "obj:0:comdat:main");
  ASSERT_EQ(loadedText->redirects.size(), 1u);
  EXPECT_EQ(loadedText->redirects[0].canonicalSymbol, "main");
  EXPECT_EQ(loadedText->redirects[0].redirectCapacity, 16u);
  EXPECT_EQ(loadedText->thunkPool.poolStartRVA, 0x1800u);
  EXPECT_EQ(loadedText->thunkPool.poolEndRVA, 0x1A00u);
  EXPECT_EQ(loadedText->thunkPool.nextFreeRVA, 0x1A00u);

  const IncrementalPackedPrefixSectionSnapshot *loadedPacked =
      findPackedPrefixSectionSnapshot(loaded->sections[1]);
  ASSERT_NE(loadedPacked, nullptr);
  EXPECT_EQ(loadedPacked->section.name, ".pdata");
  EXPECT_EQ(loadedPacked->activePrefixSize, 32u);
  EXPECT_EQ(loadedPacked->reserveSize, 16u);
  ASSERT_EQ(loadedPacked->members.size(), 1u);
  EXPECT_EQ(loadedPacked->members[0].key, "obj:0:comdat:main");
}

TEST_F(IncrementalStateTest, RoundTripPreservesResolvedSymbolWireVariants) {
  IncrementalBaselineSnapshot state;
  state.machine = AMD64;
  state.outputPath = "out.exe";

  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ObjFileRegularResolvedSymbol>(
          ObjFileRegularResolvedSymbol{"objReg", 7, 11, "obj:7:reg"}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<BitcodeRegularResolvedSymbol>(
          BitcodeRegularResolvedSymbol{"bcReg", 22}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ObjFileCommonResolvedSymbol>(
          ObjFileCommonResolvedSymbol{"objCommon", 3, 33, 16}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<BitcodeCommonResolvedSymbol>(
          BitcodeCommonResolvedSymbol{"bcCommon", 44, 32}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ImportDataResolvedSymbol>(
          ImportDataResolvedSymbol{"impData", 55, "KERNEL32.dll",
                                   "ExitProcess", 66}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ImportThunkResolvedSymbol>(
          ImportThunkResolvedSymbol{"impThunk", "ExitProcess"}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<LocalImportResolvedSymbol>(
          LocalImportResolvedSymbol{"localImp", "local:chunk"}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<AbsoluteResolvedSymbol>(
          AbsoluteResolvedSymbol{"abs", 77}));
  state.symbols.push_back(IncrementalResolvedSymbolSnapshot::make<
                          ChunkBackedSyntheticResolvedSymbol>(
      ChunkBackedSyntheticResolvedSymbol{"synthetic", "syn:chunk"}));
  state.symbols.push_back(
      IncrementalResolvedSymbolSnapshot::make<ImageBaseSyntheticResolvedSymbol>(
          ImageBaseSyntheticResolvedSymbol{"__ImageBase"}));

  SmallString<128> path = getPath("symbol-variants.llilk");
  expectNoError(writeIncrementalState(path, state));

  Expected<IncrementalBaselineSnapshot> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());

  ASSERT_EQ(loaded->sections.size(), 0u);
  ASSERT_EQ(loaded->symbols.size(), 10u);
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[0]),
            "objreg:objReg:7:11:obj:7:reg");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[1]), "bcreg:bcReg:22");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[2]),
            "objcommon:objCommon:3:33:16");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[3]),
            "bccommon:bcCommon:44:32");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[4]),
            "importdata:impData:55:KERNEL32.dll:ExitProcess:66");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[5]),
            "importthunk:impThunk:ExitProcess");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[6]),
            "localimport:localImp:local:chunk");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[7]),
            "absolute:abs:77");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[8]),
            "chunksynth:synthetic:syn:chunk");
  EXPECT_EQ(describeResolvedSymbolForTest(loaded->symbols[9]),
            "imagebase:__ImageBase");
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

  Expected<IncrementalBaselineSnapshot> badMagic = loadIncrementalState(path);
  ASSERT_FALSE(static_cast<bool>(badMagic));
  consumeError(badMagic.takeError());

  IncrementalBaselineSnapshot state;
  state.machine = AMD64;
  state.outputPath = "out.exe";
  expectNoError(writeIncrementalState(path, state));

  auto bufferOrErr = MemoryBuffer::getFile(path, /*IsText=*/false,
                                           /*RequiresNullTerminator=*/false);
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

  Expected<IncrementalBaselineSnapshot> badVersion = loadIncrementalState(path);
  ASSERT_FALSE(static_cast<bool>(badVersion));
  consumeError(badVersion.takeError());
}

TEST(IncrementalHelpersTest, BestFitSelectionHonorsCapacityAndAlignment) {
  std::vector<IncrementalPreservedSlot> slots;
  slots.push_back(IncrementalPreservedSlot::make<FreeSlotRecord>(
      FreeSlotRecord{IncrementalPreservedSlotState{0x1000, 32, 0, 1, 0}}));
  slots.push_back(IncrementalPreservedSlot::make<FreeSlotRecord>(
      FreeSlotRecord{IncrementalPreservedSlotState{0x1010, 24, 0, 1, 0}}));
  slots.push_back(IncrementalPreservedSlot::make<FreeSlotRecord>(
      FreeSlotRecord{IncrementalPreservedSlotState{0x1024, 24, 0, 1, 0}}));

  std::optional<size_t> slot = getSelectedFreeSlotIndex(slots, 16, 16);
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 1u);

  slot = getSelectedFreeSlotIndex(slots, 16, 32);
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 0u);

  EXPECT_FALSE(getSelectedFreeSlotIndex(slots, 64, 16).has_value());
}

TEST(IncrementalHelpersTest, TailReserveAllocationAlignsAndRejectsOverflow) {
  std::optional<uint64_t> start = getTailReserveStartRVA(0x1003, 0x1010, 4, 4);
  ASSERT_TRUE(start.has_value());
  EXPECT_EQ(*start, 0x1004u);

  EXPECT_FALSE(getTailReserveStartRVA(0x100f, 0x1010, 4, 4).has_value());
}

TEST(IncrementalHelpersTest, Amd64Rel32RangeHelperChecksBoundaries) {
  uint64_t source = 0x1000;
  uint64_t maxInRange = source + 4 + uint64_t(INT32_MAX);
  EXPECT_TRUE(isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32,
                                             source, maxInRange));
  EXPECT_FALSE(isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32,
                                              source, maxInRange + 1));

  uint64_t minInRange = source + 4 - uint64_t(0x80000000ULL);
  EXPECT_TRUE(isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32,
                                             source, minInRange));
  EXPECT_FALSE(isIncrementalAmd64Rel32InRange(llvm::COFF::IMAGE_REL_AMD64_REL32,
                                              source, minInRange - 1));
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
  EXPECT_FALSE(isIncrementalPersistedSlotChunk(
      IncrementalSectionLayoutKind::TextFreeSlots, bodyChunk));
}

TEST(IncrementalHelpersTest, ChooseTextThunkRVAReusesValidExistingSlot) {
  std::optional<uint64_t> thunkRVA =
      getSelectedPoolThunkRVA(0x2ff0, 0x2400, 0x2fe0, 0x3000, {});
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x2ff0u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAAllocatesFreshSlotWhenExistingOneIsInvalid) {
  std::optional<uint64_t> thunkRVA =
      getSelectedPoolThunkRVA(0x2fe0, 0x2ff8, 0x3010, 0x3020, {});
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x3000u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAReusesFreedSlotBeforeScanningBelowPoolCursor) {
  uint64_t freedThunkRVAs[] = {0x2fd0, 0x2ff0};
  std::optional<uint64_t> thunkRVA =
      getSelectedPoolThunkRVA(0, 0x2fc0, 0x2fd0, 0x3000, {}, freedThunkRVAs);
  ASSERT_TRUE(thunkRVA.has_value());
  EXPECT_EQ(*thunkRVA, 0x2ff0u);
}

TEST(IncrementalHelpersTest,
     ChooseTextThunkRVAIgnoresClaimedOrInvalidFreedSlots) {
  uint64_t claimedThunkRVAs[] = {0x2ff0};
  uint64_t freedThunkRVAs[] = {0x2fb0, 0x2ff0, 0x2fd0};
  std::optional<uint64_t> thunkRVA = getSelectedPoolThunkRVA(
      0, 0x2fc0, 0x2fd0, 0x3000, claimedThunkRVAs, freedThunkRVAs);
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
  EXPECT_EQ(plans[0].targeting, IncrementalRedirectTargeting::DirectBodyTarget);
  EXPECT_EQ(plans[0].poolThunkRVA, 0u);

  EXPECT_EQ(plans[1].engagement, IncrementalRedirectEngagement::Installed);
  EXPECT_EQ(plans[1].targeting, IncrementalRedirectTargeting::PoolThunkTarget);
  EXPECT_EQ(plans[1].poolThunkRVA, 0x2ff0u);
  EXPECT_EQ(poolCursor, 0x2fd0u);
  EXPECT_EQ(poolStart, 0x2ff0u);
}

TEST(IncrementalHelpersTest, ChooseTextThunkRVAFailsWhenPoolIsExhausted) {
  EXPECT_FALSE(
      getSelectedPoolThunkRVA(0x2ff0, 0x2ff8, 0x3000, 0x3000, {}).has_value());
}

TEST(IncrementalHelpersTest, RedirectStateRoundTripPreservesPoolState) {
  IncrementalBaselineSnapshot state;
  state.outputPath = "out.exe";

  IncrementalTextRedirectState redirect;
  redirect.targetKey = "obj:0:comdat:target";
  redirect.canonicalSymbol = "target";
  redirect.redirectRVA = 0x1200;
  redirect.redirectCapacity = 16;
  redirect.bodyRVA = 0x2400;
  redirect.poolThunkRVA = 0x4FF0;

  IncrementalSectionState section;
  section.name = ".text";
  section.characteristics = 0x60000020;
  TextSlotSectionSnapshot textSection;
  textSection.slotSection.section = section;
  textSection.redirects.push_back(redirect);
  textSection.thunkPool.poolStartRVA = 0x4000;
  textSection.thunkPool.poolEndRVA = 0x5000;
  textSection.thunkPool.nextFreeRVA = 0x4FF0;
  state.sections.push_back(
      IncrementalSectionSnapshot::make<TextSlotSectionSnapshot>(
          std::move(textSection)));

  SmallString<128> path;
  ASSERT_FALSE(sys::fs::createTemporaryFile("redirect-pool", "llilk", path));
  Error err = writeIncrementalState(path, state);
  ASSERT_FALSE(static_cast<bool>(err)) << toString(std::move(err));
  Expected<IncrementalBaselineSnapshot> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());
  ASSERT_EQ(loaded->sections.size(), 1u);
  const TextSlotSectionSnapshot *loadedText =
      findTextSlotSectionSnapshot(loaded->sections[0]);
  ASSERT_NE(loadedText, nullptr);
  ASSERT_EQ(loadedText->redirects.size(), 1u);
  EXPECT_EQ(loadedText->redirects[0].poolThunkRVA, 0x4FF0u);
  EXPECT_EQ(loadedText->thunkPool.poolStartRVA, 0x4000u);
  EXPECT_EQ(loadedText->thunkPool.poolEndRVA, 0x5000u);
  EXPECT_EQ(loadedText->thunkPool.nextFreeRVA, 0x4FF0u);
  std::error_code ec = sys::fs::remove(path);
  EXPECT_FALSE(ec);
}

} // namespace
