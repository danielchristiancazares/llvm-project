#include "../../COFF/IncrementalState.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

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

TEST_F(IncrementalStateTest, RoundTripPreservesExtendedFields) {
  IncrementalStateFile state;
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

  SmallString<128> path = getPath("state.llilk");
  expectNoError(writeIncrementalState(path, state));

  Expected<IncrementalStateFile> loaded = loadIncrementalState(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << toString(loaded.takeError());

  EXPECT_EQ(loaded->version, 2u);
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

} // namespace
