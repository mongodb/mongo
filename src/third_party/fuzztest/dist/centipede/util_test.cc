// Copyright 2022 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./centipede/util.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT
#include <map>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/container/flat_hash_map.h"
#include "absl/log/log.h"
#include "./centipede/feature.h"
#include "./centipede/thread_pool.h"
#include "./common/defs.h"
#include "./common/hash.h"

namespace fuzztest::internal {

TEST(UtilTest, AsString) {
  EXPECT_EQ(AsPrintableString({'a', 'b', 'c'}, 3), "abc");
  EXPECT_EQ(AsPrintableString({'a', 'b', 'C'}, 4), "abC");
  EXPECT_EQ(AsPrintableString({'a', 'b', 'c'}, 2), "ab");
  // NOTE: Test both int (0xAB) and char ('\xAB') literals as ByteArray
  // initializers: the latter used to cause compilation failures with
  // Bazel/Clang default setup (without --cxxopt=--fno-signed-char in .bazelrc).
  EXPECT_EQ(AsPrintableString({'a', 0xAB, 0xCD}, 3), "a\\xAB\\xCD");
  EXPECT_EQ(AsPrintableString({'a', 0xAB, 0xCD}, 4), "a\\xAB\\xCD");
  EXPECT_EQ(AsPrintableString({'a', '\xAB', '\xCD'}, 2), "a\\xAB");
  EXPECT_EQ(AsPrintableString({'a', '\xAB', '\xCD', 'z'}, 5), "a\\xAB\\xCDz");
}

TEST(UtilTest, ExtractHashFromArray) {
  const ByteArray a{1, 2, 3, 4};
  const ByteArray b{100, 111, 122, 133, 145};
  auto hash1 = Hash({4, 5, 6});
  auto hash2 = Hash({7, 8});

  ByteArray a1 = a;
  AppendHashToArray(a1, hash1);
  EXPECT_EQ(a1.size(), a.size() + hash1.size());

  ByteArray b2 = b;
  AppendHashToArray(b2, hash2);
  EXPECT_EQ(b2.size(), b.size() + hash2.size());

  EXPECT_EQ(ExtractHashFromArray(b2), hash2);
  EXPECT_EQ(b2, b);

  EXPECT_EQ(ExtractHashFromArray(a1), hash1);
  EXPECT_EQ(a1, a);
}

TEST(UtilTest, PackAndUnpackFeatures) {
  const ByteArray kData{1, 2, 3, 4};
  const FeatureVec kFeatures = {102, 30, 7, 15};
  std::string hash = Hash(kData);
  ByteArray packed = PackFeaturesAndHash(kData, kFeatures);

  FeatureVec unpacked_features;
  std::string unpacked_hash = UnpackFeaturesAndHash(packed, &unpacked_features);
  EXPECT_EQ(kFeatures, unpacked_features);
  EXPECT_EQ(hash, unpacked_hash);
}

TEST(UtilTest, PackAndUnpackEmptyFeatures) {
  const ByteArray kData{1, 2, 3, 4};
  std::string hash = Hash(kData);
  ByteArray packed = PackFeaturesAndHash(kData, {});

  FeatureVec unpacked_features;
  std::string unpacked_hash = UnpackFeaturesAndHash(packed, &unpacked_features);
  EXPECT_TRUE(unpacked_features.empty());
  EXPECT_EQ(hash, unpacked_hash);
}

TEST(UtilTest, PackAndUnpackFeaturesAsRawBytes) {
  const ByteArray kData{1, 2, 3, 4};
  std::string hash = Hash(kData);
  const FeatureVec kFeatures = {102, 30, 7, 15};
  ByteArray packed =
      PackFeaturesAndHashAsRawBytes(kData, AsByteSpan(kFeatures));
  FeatureVec unpacked_features;
  std::string unpacked_hash = UnpackFeaturesAndHash(packed, &unpacked_features);
  EXPECT_EQ(kFeatures, unpacked_features);
  EXPECT_EQ(hash, unpacked_hash);
}

// Tests TemporaryLocalDirPath from several threads.
TEST(UtilTest, TemporaryLocalDirPath) {
  {
    // Check that repeated calls return the same path.
    auto temp_dir = TemporaryLocalDirPath();
    LOG(INFO) << temp_dir;
    EXPECT_EQ(temp_dir, TemporaryLocalDirPath());
  }

  {
    auto temp_dir = TemporaryLocalDirPath();
    // Create dir, create a file there, write to file, read from it, remove dir.
    std::filesystem::create_directories(temp_dir);
    std::string temp_file_path = std::filesystem::path(temp_dir).append("blah");
    ByteArray written_data{1, 2, 3};
    WriteToLocalFile(temp_file_path, written_data);
    ByteArray read_data;
    ReadFromLocalFile(temp_file_path, read_data);
    EXPECT_EQ(read_data, written_data);
    std::filesystem::remove_all(temp_dir);
    // temp_file_path should be gone by now.
    read_data.clear();
    ReadFromLocalFile(temp_file_path, read_data);
    EXPECT_TRUE(read_data.empty());
  }

  {
    // Create dirs in two threads.
    std::string temp_dir_1, temp_dir_2;
    {
      // NOTE: Not using a 2-threaded pool here because the scheduled tasks are
      // too fast and can end up both running in the same thread. Also, not
      // using `std::thread`.
      ThreadPool thread1{1}, thread2{1};
      thread1.Schedule(
          [&temp_dir_1]() { temp_dir_1 = TemporaryLocalDirPath(); });
      thread2.Schedule(
          [&temp_dir_2]() { temp_dir_2 = TemporaryLocalDirPath(); });
    }  // The threads join here.
    EXPECT_NE(temp_dir_1, temp_dir_2);
  }
}

TEST(UtilTest, CreateLocalDirRemovedAtExit) {
  // We need to test that dirs created via CreateLocalDirRemovedAtExit
  // are removed at exit.
  // To do that, we run death tests and check if the dirs exist afterwards.
  // The path to directory is computed in the parent test, then it is
  // passed via an env. var. to the child test so that the child test doesn't
  // recompute it to be something different.
  const char *centipede_util_test_temp_dir =
      getenv("CENTIPEDE_UTIL_TEST_TEMP_DIR");
  auto tmpdir = centipede_util_test_temp_dir ? centipede_util_test_temp_dir
                                             : TemporaryLocalDirPath();
  EXPECT_FALSE(std::filesystem::exists(tmpdir));
  CreateLocalDirRemovedAtExit(tmpdir);
  EXPECT_TRUE(std::filesystem::exists(tmpdir));
  setenv("CENTIPEDE_UTIL_TEST_TEMP_DIR", tmpdir.c_str(), 1);
  // Create two subdirs via CreateLocalDirRemovedAtExit.
  std::string subdir1 = std::filesystem::path(tmpdir).append("1");
  std::string subdir2 = std::filesystem::path(tmpdir).append("2");
  CreateLocalDirRemovedAtExit(subdir1);
  CreateLocalDirRemovedAtExit(subdir2);
  EXPECT_TRUE(std::filesystem::exists(subdir1));
  EXPECT_TRUE(std::filesystem::exists(subdir2));

  // Run a subprocess that creates the same two subdirs and ends with abort.
  // Both subdirs should still be there.
  auto create_dir_and_abort = [&]() {
    CreateLocalDirRemovedAtExit(subdir1);
    CreateLocalDirRemovedAtExit(subdir2);
    abort();  // atexit handlers are not called.
  };
  EXPECT_DEATH(create_dir_and_abort(), "");
  EXPECT_TRUE(std::filesystem::exists(subdir1));
  EXPECT_TRUE(std::filesystem::exists(subdir2));

  // Run a subprocess that creates the same two subdirs and ends with exit.
  // Both subdirs should be gone.
  auto create_dir_and_exit1 = [&]() {
    CreateLocalDirRemovedAtExit(subdir1);
    CreateLocalDirRemovedAtExit(subdir2);
    exit(1);  // atexit handlers are called.
  };
  EXPECT_DEATH(create_dir_and_exit1(), "");
  EXPECT_FALSE(std::filesystem::exists(subdir1));
  EXPECT_FALSE(std::filesystem::exists(subdir2));
}

TEST(UtilTest, ParseAFLDictionary) {
  std::vector<ByteArray> dict;
  EXPECT_TRUE(ParseAFLDictionary("", dict));                      // Empty text.
  EXPECT_FALSE(ParseAFLDictionary("\xAB", dict));                 // Non-ascii.
  EXPECT_FALSE(ParseAFLDictionary(" l1  \n\t\t\tl2  \n", dict));  // Missing "
  EXPECT_FALSE(ParseAFLDictionary(" \"zzz", dict));  // Missing second "

  // Two entries and a comment.
  EXPECT_TRUE(
      ParseAFLDictionary("  name=\"v1\"  \n"
                         " # comment\n"
                         " \"v2\"",
                         dict));
  EXPECT_EQ(dict, std::vector<ByteArray>({{'v', '1'}, {'v', '2'}}));

  // Hex entries and a properly escaped backslash.
  EXPECT_TRUE(ParseAFLDictionary("  \"\\xBC\\\\a\\xAB\\x00\"", dict));
  EXPECT_EQ(dict, std::vector<ByteArray>({{'\xBC', '\\', 'a', '\xAB', 0}}));

  // Special characters.
  EXPECT_TRUE(ParseAFLDictionary("\"\\r\\t\\n\\\"\"", dict));
  EXPECT_EQ(dict, std::vector<ByteArray>({{'\r', '\t', '\n', '"'}}));

  // Improper use of backslash, still parses.
  EXPECT_TRUE(ParseAFLDictionary("\"\\g\\h\"", dict));
  EXPECT_EQ(dict, std::vector<ByteArray>({{'\\', 'g', '\\', 'h'}}));
}

TEST(UtilTest, RandomWeightedSubset) {
  using v = std::vector<size_t>;  // to make test code more compact.
  std::vector<uint64_t> set{20, 10, 0, 40, 50};
  Rng rng(0);

  // target_size >= 4, expect only the index of 0s.
  EXPECT_THAT(RandomWeightedSubset(set, 10, rng), testing::ElementsAre(2));
  EXPECT_THAT(RandomWeightedSubset(set, 4, rng), testing::ElementsAre(2));

  // For more interesting values of target_size, run many iterations, sort
  // results by frequency, verify that more likely results are more frequent.
  constexpr size_t kNumIter = 100000;

  // Maps a result to its frequency.
  absl::flat_hash_map<std::vector<size_t>, size_t> results;

  // Returns a vector of results ordered from least frequent to most frequent.
  auto order_results = [&]() {
    std::vector<std::vector<size_t>> ordered_results;
    std::map<size_t, std::vector<size_t>> freq_to_res;
    for (const auto &it : results) freq_to_res[it.second] = it.first;
    ordered_results.reserve(freq_to_res.size());
    for (const auto &it : freq_to_res) ordered_results.push_back(it.second);
    return ordered_results;
  };

  // target size: 3
  for (size_t i = 0; i < kNumIter; ++i) {
    ++results[RandomWeightedSubset(set, /*target_size=*/3, rng)];
  }
  EXPECT_THAT(order_results(),
              testing::ElementsAre(v{2, 4}, v{2, 3}, v{0, 2}, v{1, 2}));

  // target_size: 2
  results.clear();
  for (size_t i = 0; i < kNumIter; ++i) {
    ++results[RandomWeightedSubset(set, /*target_size=*/2, rng)];
  }
  EXPECT_THAT(order_results(),
              testing::ElementsAre(v{2, 3, 4}, v{0, 2, 4}, v{0, 2, 3},
                                   v{1, 2, 4}, v{1, 2, 3}, v{0, 1, 2}));
  // target_size: 1
  results.clear();
  for (size_t i = 0; i < kNumIter; ++i) {
    ++results[RandomWeightedSubset(set, /*target_size=*/1, rng)];
  }
  EXPECT_THAT(order_results(),
              testing::ElementsAre(v{0, 2, 3, 4}, v{1, 2, 3, 4}, v{0, 1, 2, 4},
                                   v{0, 1, 2, 3}));
}

TEST(UtilTest, RemoveSubset) {
  std::vector<int> set;
  auto Remove = [](const std::vector<size_t> &subset_indices,
                   std::vector<int> set) {
    RemoveSubset(subset_indices, set);
    return set;
  };
  EXPECT_THAT(Remove({0, 1}, {10, 20, 30, 40}), testing::ElementsAre(30, 40));
  EXPECT_THAT(Remove({1, 2}, {10, 20, 30, 40}), testing::ElementsAre(10, 40));
  EXPECT_THAT(Remove({2, 3}, {10, 20, 30, 40}), testing::ElementsAre(10, 20));
  EXPECT_THAT(Remove({1}, {10, 20, 30, 40}), testing::ElementsAre(10, 30, 40));
  EXPECT_THAT(Remove({}, {10, 20, 30, 40}),
              testing::ElementsAre(10, 20, 30, 40));
  EXPECT_THAT(Remove({0, 1, 2, 3}, {10, 20, 30, 40}), testing::IsEmpty());

  // Check that RemoveSubset can be applied to a vector.
  std::vector<std::vector<int>> vector_set = {{1}, {2}, {3}};
  RemoveSubset({1}, vector_set);
  EXPECT_THAT(vector_set,
              testing::ElementsAre(std::vector<int>{1}, std::vector<int>{3}));
}

}  // namespace fuzztest::internal
