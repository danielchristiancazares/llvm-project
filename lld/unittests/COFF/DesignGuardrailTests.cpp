#include "lld/Common/Closed.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"
#include <regex>
#include <string>
#include <system_error>

#ifndef LLD_TEST_REPO_ROOT
#error "LLD_TEST_REPO_ROOT must be defined for design guardrail tests"
#endif

using namespace llvm;
using namespace lld;

namespace {

struct FirstAlternative final {
  int value;
};

struct SecondAlternative final {
  int value;
};

using ExampleClosed = Closed<FirstAlternative, SecondAlternative>;

static_assert(!std::is_default_constructible_v<ExampleClosed>);
static_assert(!std::is_copy_constructible_v<ExampleClosed>);
static_assert(!std::is_copy_assignable_v<ExampleClosed>);
static_assert(std::is_nothrow_move_constructible_v<ExampleClosed>);
static_assert(!std::is_move_assignable_v<ExampleClosed>);

static std::string readRepoFileOrEmpty(StringRef relativePath) {
  SmallString<256> path(LLD_TEST_REPO_ROOT);
  sys::path::append(path, relativePath);
  auto bufferOrErr = MemoryBuffer::getFile(path);
  if (!bufferOrErr) {
    ADD_FAILURE() << "failed to open " << path.c_str() << ": "
                  << bufferOrErr.getError().message();
    return {};
  }
  return (*bufferOrErr)->getBuffer().str();
}

static size_t countMatchingLines(StringRef contents, const std::regex &pattern) {
  SmallVector<StringRef, 0> lines;
  contents.split(lines, '\n');

  size_t count = 0;
  for (StringRef line : lines) {
    std::string copy = line.str();
    if (std::regex_search(copy, pattern))
      ++count;
  }
  return count;
}

static size_t countMatchingLinesInCoffHeaders(const std::regex &pattern) {
  SmallString<256> headerDir(LLD_TEST_REPO_ROOT);
  sys::path::append(headerDir, "lld", "COFF");

  size_t count = 0;
  std::error_code ec;
  for (sys::fs::directory_iterator it(headerDir, ec), end; it != end && !ec;
       it.increment(ec)) {
    StringRef path = it->path();
    if (sys::path::extension(path) != ".h")
      continue;

    auto bufferOrErr = MemoryBuffer::getFile(path);
    if (!bufferOrErr) {
      ADD_FAILURE() << "failed to open " << path.str() << ": "
                    << bufferOrErr.getError().message();
      return 0;
    }
    count += countMatchingLines((*bufferOrErr)->getBuffer(), pattern);
  }

  if (ec) {
    ADD_FAILURE() << "failed to iterate " << headerDir.c_str() << ": "
                  << ec.message();
    return 0;
  }
  return count;
}

static size_t countMatchingLinesInFiles(ArrayRef<StringRef> relativePaths,
                                        const std::regex &pattern) {
  size_t count = 0;
  for (StringRef relativePath : relativePaths)
    count += countMatchingLines(readRepoFileOrEmpty(relativePath), pattern);
  return count;
}

TEST(ClosedDesignTest, MatchReturnsActiveAlternativeForLValue) {
  ExampleClosed value = ExampleClosed::make<SecondAlternative>(
      SecondAlternative{42});
  EXPECT_EQ(value.match([](FirstAlternative &) { return 1; },
                        [](SecondAlternative &second) { return second.value; }),
            42);
}

TEST(ClosedDesignTest, MatchReturnsActiveAlternativeAfterMoveConstruction) {
  ExampleClosed original =
      ExampleClosed::make<FirstAlternative>(FirstAlternative{7});
  ExampleClosed moved(std::move(original));
  EXPECT_EQ(std::move(moved).match(
                [](FirstAlternative &&first) { return first.value; },
                [](SecondAlternative &&) { return -1; }),
            7);
}

TEST(DesignGuardrailTest, CoffHeadersDoNotAddMoreBoolLines) {
  EXPECT_LE(countMatchingLinesInCoffHeaders(std::regex(R"(\bbool\b)")), 182u);
}

TEST(DesignGuardrailTest, CoffHeadersDoNotAddMoreOptionalLines) {
  EXPECT_LE(
      countMatchingLinesInCoffHeaders(std::regex(R"(std::optional|optional<)")),
      15u);
}

TEST(DesignGuardrailTest, CoffHeadersDoNotAddFriendClassLines) {
  EXPECT_LE(countMatchingLinesInCoffHeaders(std::regex(R"(friend class)")),
            2u);
}

TEST(DesignGuardrailTest, CoffHeadersDoNotAddMoreNullabilityLines) {
  EXPECT_LE(
      countMatchingLinesInCoffHeaders(std::regex(R"(= nullptr|\bnullptr\b)")),
      64u);
}

TEST(DesignGuardrailTest, CoffHeadersDoNotAddVariantLines) {
  EXPECT_LE(
      countMatchingLinesInCoffHeaders(std::regex(R"(std::variant|variant<)")),
      0u);
}

TEST(DesignGuardrailTest, SelectedCoreModulesDoNotAddAssertLines) {
  static const StringRef files[] = {"lld/COFF/Driver.cpp",
                                    "lld/COFF/InputFiles.cpp",
                                    "lld/COFF/LTO.cpp"};
  EXPECT_LE(countMatchingLinesInFiles(files, std::regex(R"(\bassert\s*\()")),
            7u);
}

TEST(DesignGuardrailTest, SelectedCoreModulesDoNotAddValueLines) {
  static const StringRef files[] = {"lld/COFF/Driver.cpp",
                                    "lld/COFF/InputFiles.cpp",
                                    "lld/COFF/LTO.cpp"};
  EXPECT_LE(countMatchingLinesInFiles(files, std::regex(R"(\.value\()")), 0u);
}

TEST(DesignGuardrailTest, SelectedCoreModulesDoNotAddValueOrLines) {
  static const StringRef files[] = {"lld/COFF/Driver.cpp",
                                    "lld/COFF/InputFiles.cpp",
                                    "lld/COFF/LTO.cpp"};
  EXPECT_LE(countMatchingLinesInFiles(files, std::regex(R"(\.value_or\()")),
            1u);
}

TEST(DesignGuardrailTest, SelectedCoreModulesDoNotAddSentinelPointerLines) {
  static const StringRef files[] = {"lld/COFF/Driver.cpp",
                                    "lld/COFF/InputFiles.cpp",
                                    "lld/COFF/LTO.cpp"};
  EXPECT_LE(countMatchingLinesInFiles(
                files, std::regex(R"(reinterpret_cast<[^>]+>\(1\))")),
            1u);
}

TEST(DesignGuardrailTest, ClosedHeaderRetainsNothrowMoveContract) {
  std::string contents = readRepoFileOrEmpty("lld/include/lld/Common/Closed.h");
  EXPECT_NE(contents.find("is_nothrow_move_constructible_v"),
            std::string::npos);
  EXPECT_EQ(contents.find("Closed(Closed &&) = default"), std::string::npos);
}

} // namespace
