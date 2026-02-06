#include "./fuzztest/fuzztest_macros.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/match.h"
#include "./common/temp_dir.h"

namespace fuzztest::internal {
namespace {

namespace fs = std::filesystem;

using ::testing::ElementsAre;
using ::testing::FieldsAre;
using ::testing::UnorderedElementsAre;

void WriteFile(const std::string& file_name, const std::string& contents) {
  std::ofstream f(file_name);
  CHECK(f.is_open());
  f << contents;
  f.close();
}

TEST(ReadFilesFromDirectoryTest, NoFilterReturnsEverything) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "file-1.txt", "content-1");
  WriteFile(temp_dir.path() / "file-2.txt", "content-2");

  auto seeds = ReadFilesFromDirectory(
      temp_dir.path().c_str(), [](std::string_view name) { return true; });

  EXPECT_THAT(seeds, UnorderedElementsAre(FieldsAre("content-1"),
                                          FieldsAre("content-2")));
}

TEST(ReadFilesFromDirectoryTest, DirectoryIsTraversedRecursively) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "file-1.txt", "content-1");
  fs::create_directories(temp_dir.path() / "sub-dir");
  WriteFile(temp_dir.path() / "sub-dir" / "file-2.txt", "content-2");

  auto seeds = ReadFilesFromDirectory(
      temp_dir.path().c_str(), [](std::string_view name) { return true; });

  EXPECT_THAT(seeds, UnorderedElementsAre(FieldsAre("content-1"),
                                          FieldsAre("content-2")));
}

TEST(ReadFilesFromDirectoryTest, FilterReturnsOnlyMatchingFiles) {
  TempDir temp_dir;
  WriteFile(temp_dir.path() / "file.png", "image");
  WriteFile(temp_dir.path() / "file.txt", "text");

  auto seeds = ReadFilesFromDirectory(
      temp_dir.path().c_str(),
      [](std::string_view name) { return absl::EndsWith(name, ".png"); });

  EXPECT_THAT(seeds, UnorderedElementsAre(FieldsAre("image")));
}

TEST(ReadFilesFromDirectoryTest, DiesOnInvalidDirectory) {
  EXPECT_DEATH(ReadFilesFromDirectory(
                   "invalid_dir", [](std::string_view name) { return true; }),
               "Not a directory: invalid_dir");
}

TEST(ParseDictionaryTest, Success) {
  // Derived from https://llvm.org/docs/LibFuzzer.html#dictionaries
  std::string dictionary_content =
      R"(# Lines starting with '#' and empty lines are ignored.

# Adds "blah" (w/o quotes) to the dictionary.
kw1="blah"
# Use \\ for backslash and \" for quotes.
kw2="\"ac\\dc\""
# Use \xAB for hex values
kw3="\xF7\xF8"
# the name of the keyword followed by '=' may be omitted:
"foo\x0Abar"

# Null character is unescaped as well
"foo\x00bar"
)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  ASSERT_TRUE(dictionary_entries.ok());
  EXPECT_THAT(*dictionary_entries,
              ElementsAre("blah", "\"ac\\dc\"", "\xF7\xF8", "foo\nbar",
                          std::string("foo\0bar", 7)));
}
TEST(ParseDictionaryTest, FailsWithNoQuote) {
  std::string dictionary_content = R"(kw1=world)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(dictionary_entries.status().message(),
              "Unparseable dictionary entry at line 1: missing quotes");
}

TEST(ParseDictionaryTest, FailsWithNoClosingQuote) {
  std::string dictionary_content = R"(kw1="world)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(dictionary_entries.status().message(),
              "Unparseable dictionary entry at line 1: entry must be enclosed "
              "in quotes");
}

TEST(ParseDictionaryTest, FailsWithInvalidEscapeSequence) {
  std::string dictionary_content = R"(
# Valid
kw1="Hello"

# Invalid
kw2="world\!"
)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(dictionary_entries.status().message(),
              "Unparseable dictionary entry at line 6: Invalid escape sequence "
              "in dictionary entry: \\!");
}

TEST(ParseDictionaryTest, FailsWithEmptyHexEscapeSequence) {
  std::string dictionary_content = R"(
# Valid
kw1="Hello"

# Invalid
kw2="world\x"
)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(dictionary_entries.status().message(),
              "Unparseable dictionary entry at line 6: Invalid escape sequence "
              "in dictionary entry: \\x");
}

TEST(ParseDictionaryTest, FailsWithHexEscapeSequenceWithSingleDigit) {
  std::string dictionary_content = R"(
# Valid
kw1="Hello"

# Invalid
kw2="world\x2"
)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(dictionary_entries.status().message(),
              "Unparseable dictionary entry at line 6: Invalid escape sequence "
              "in dictionary entry: \\x");
}

TEST(ParseDictionaryTest, FailsWithInvalidTwoDigitHexEscapeSequence) {
  std::string dictionary_content = R"(
# Valid
kw1="Hello"

# Invalid
kw2="world\x5g"
)";
  absl::StatusOr<std::vector<std::string>> dictionary_entries =
      ParseDictionary(dictionary_content);
  EXPECT_EQ(dictionary_entries.status().code(),
            absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(
      dictionary_entries.status().message(),
      "Unparseable dictionary entry at line 6: Could not unescape \\x5g");
}

}  // namespace
}  // namespace fuzztest::internal
