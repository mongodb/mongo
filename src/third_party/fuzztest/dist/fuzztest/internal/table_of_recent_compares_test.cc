
#include "./fuzztest/internal/table_of_recent_compares.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/random/random.h"

namespace fuzztest::internal {
namespace {

// TODO(JunyangShao) : Make these functions neater (https://abseil.io/tips/122).
TablesOfRecentCompares GetFilledIntegerTORC() {
  TablesOfRecentCompares table = {};
  // Fill container TORC and uint32 TORC,
  // duplicate 8 to test dedup.
  for (int i = 0; i < 8; ++i) {
    table.GetMutable<4>().Insert(1234, 5678);
  }
  // Fill uint32_t TORC with casted values.
  table.GetMutable<4>().Insert(-10, -5);
  return table;
}

// TODO(JunyangShao) : Make these functions neater (https://abseil.io/tips/122).
TablesOfRecentCompares GetFilledContainerTORC() {
  TablesOfRecentCompares table = {};
  std::string lhs = "\x11\x11\x22\x22";
  std::string rhs = "\x33\x33\x44\x44";
  for (int i = 0; i < 8; ++i) {
    table.GetMutable<0>().Insert(reinterpret_cast<const uint8_t*>(lhs.data()),
                                 reinterpret_cast<const uint8_t*>(rhs.data()),
                                 4);
  }

  // Fill uint64_t TORC with cast-from-string values. Which could be
  // matched by a ContainerDictionary. To test cast-int-to-buffer matching.
  lhs = "\xaa\xaa\xbb\xbb";
  rhs = "\xcc\xcc\xdd\xdd";
  uint32_t uint64_lhs = *(reinterpret_cast<const uint32_t*>(lhs.data()));
  uint32_t uint64_rhs = *(reinterpret_cast<const uint32_t*>(rhs.data()));
  table.GetMutable<8>().Insert(uint64_lhs, uint64_rhs);
  return table;
}

// TODO(JunyangShao) : Use EXPECT_THAT instead of EXPECT_EQ.
TEST(TablesOfRecentComparesTest, IntegerTORCCorrect) {
  auto table = GetFilledIntegerTORC();
  // Integer TORC can match and can dedup.
  auto v = table.Get<4>().GetMatchingIntegerDictionaryEntries(1234);
  EXPECT_EQ(v.size(), 1);
  EXPECT_EQ(v[0], 5678);

  // Integer TORC range check works.
  auto v_limited =
      table.Get<4>().GetMatchingIntegerDictionaryEntries(1234, 0, 5677);
  EXPECT_EQ(v_limited.size(), 0);

  // Integer TORC works with type casts.
  int magic_value = -10;
  auto v_casted =
      table.Get<4>().GetMatchingIntegerDictionaryEntries(magic_value);
  EXPECT_EQ(v_casted.size(), 1);
  EXPECT_EQ(v_casted[0], -5);
  auto v_casted_limited =
      table.Get<4>().GetMatchingIntegerDictionaryEntries(magic_value, -15, -11);
  EXPECT_EQ(v_casted_limited.size(), 0);
}

// TODO(JunyangShao) : Use EXPECT_THAT instead of EXPECT_EQ.
TEST(TablesOfRecentComparesTest, ContainerTORCCorrect) {
  auto table = GetFilledContainerTORC();

  // Container TORC can match and can dedup.
  std::string haystack = "\xff\xff\xff\xff\x11\x11\x22\x22\xff\xff";
  std::string needle = "\x33\x33\x44\x44";
  auto found = table.Get<0>().GetMatchingContainerDictionaryEntries(haystack);
  EXPECT_EQ(found.size(), 1);
  EXPECT_TRUE(found[0].position_hint.has_value());
  EXPECT_EQ(*found[0].position_hint, 4);
  EXPECT_EQ(found[0].value, needle);

  // Container TORC can match casted-to container values.
  std::vector<uint16_t> haystack_vec = {0xffff, 0x1111, 0x2222, 0xffff};
  std::vector<uint16_t> needle_vec = {0x3333, 0x4444};
  auto found_vec =
      table.Get<0>().GetMatchingContainerDictionaryEntries(haystack_vec);
  EXPECT_EQ(found_vec.size(), 1);
  EXPECT_TRUE(found_vec[0].position_hint.has_value());
  EXPECT_EQ(*found_vec[0].position_hint, 1);
  EXPECT_EQ(found_vec[0].value, needle_vec);
}

// TODO(JunyangShao) : Use EXPECT_THAT instead of EXPECT_EQ.
TEST(TablesOfRecentComparesTest, IntegerDictionaryCorrect) {
  auto table = GetFilledIntegerTORC();
  IntegerDictionary<uint32_t> dict = {};
  absl::BitGen bitgen;

  // Match works.
  dict.MatchEntriesFromTableOfRecentCompares(1234, table);
  EXPECT_EQ(dict.Size(), 1);
  EXPECT_EQ(dict.GetRandomSavedEntry(bitgen), 5678);

  // Get random from TORC works.
  int try_count = 10000;
  while (try_count--) {
    auto v = IntegerDictionary<uint32_t>::GetRandomTORCEntry(1234, bitgen,
                                                             table, 5000, 6000);
    if (v == 5678) break;
  }
  EXPECT_GT(try_count, 0);
}

// TODO(JunyangShao) : Use EXPECT_THAT instead of EXPECT_EQ.
TEST(TablesOfRecentComparesTest, ContainerDictionaryCorrect) {
  auto table = GetFilledContainerTORC();
  ContainerDictionary<std::string> dict = {};
  absl::BitGen bitgen;
  std::string test_string = "\x11\x11\x22\x22\xff\xff\xaa\xaa\xbb\xbb\xff\xff";

  // Match works.
  dict.MatchEntriesFromTableOfRecentCompares(test_string, table);
  bool get_3344 = false;
  bool get_ccdd = false;
  EXPECT_EQ(dict.Size(), 2);
  int try_count = 10000;
  while (try_count--) {
    auto v = dict.GetRandomSavedEntry(bitgen);
    EXPECT_TRUE(v.position_hint.has_value());
    if (v.value == "\x33\x33\x44\x44") {
      EXPECT_EQ(*v.position_hint, 0);
      get_3344 = true;
    }
    if (v.value == "\xcc\xcc\xdd\xdd") {
      EXPECT_EQ(*v.position_hint, 6);
      get_ccdd = true;
    }
    if (get_3344 && get_ccdd) break;
  }
  EXPECT_GT(try_count, 0);

  // Get random from TORC works.
  try_count = 3000;
  get_3344 = false;
  // Since ccdd is in the integer TORC which is much larger, we don't
  // check them here.
  while (try_count--) {
    auto v = dict.GetRandomTORCEntry(test_string, bitgen, table);
    if (v.has_value()) {
      if (v->position_hint.has_value()) {
        if (v->value == "\x33\x33\x44\x44") {
          EXPECT_EQ(*(v->position_hint), 0);
          break;
        }
      }
    }
  }
  EXPECT_GT(try_count, 0);
}

}  // namespace
}  // namespace fuzztest::internal
