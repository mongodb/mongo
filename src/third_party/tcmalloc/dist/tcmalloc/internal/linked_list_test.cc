// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tcmalloc/internal/linked_list.h"

#include <stddef.h>
#include <stdlib.h>

#include <algorithm>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"
#include "absl/container/flat_hash_set.h"
#include "absl/random/random.h"
#include "tcmalloc/internal/mock_span.h"

namespace tcmalloc {
namespace tcmalloc_internal {
namespace {

class LinkedListTest : public ::testing::Test {
 protected:
  LinkedList list_;

  static_assert(!std::is_copy_constructible<LinkedList>::value,
                "LinkedList should not be copyable");
  static_assert(!std::is_move_constructible<LinkedList>::value,
                "LinkedList should not be movable");
};

TEST_F(LinkedListTest, PushPop) {
  const int N = 20;
  std::vector<void*> ptrs{nullptr};

  EXPECT_EQ(0, list_.length());
  EXPECT_TRUE(list_.empty());

  for (int i = 0; i < N; i++) {
    void* ptr = malloc(sizeof(void*));
    ASSERT_FALSE(ptr == nullptr);
    ptrs.push_back(ptr);

    list_.Push(ptr);

    EXPECT_EQ(i + 1, list_.length());
    EXPECT_FALSE(list_.empty());
  }

  for (int i = N; i > 0; i--) {
    EXPECT_EQ(i, list_.length());
    EXPECT_FALSE(list_.empty());

    void* ptr;
    bool ret = list_.TryPop(&ptr);
    EXPECT_TRUE(ret);
    EXPECT_EQ(ptrs[i], ptr);

    free(ptrs[i]);
  }

  EXPECT_EQ(0, list_.length());
  EXPECT_TRUE(list_.empty());
}

// PushPopBatch validates that the batch operations push and pop the required
// number of elements from the list, but it does not assert that order within
// the batch is maintained.
TEST_F(LinkedListTest, PushPopBatch) {
  const std::vector<int> batch_sizes{1, 3, 5, 7, 10, 16};
  absl::flat_hash_set<void*> pushed;

  size_t length = 0;
  for (int batch_size : batch_sizes) {
    std::vector<void*> batch;

    for (int i = 0; i < batch_size; i++) {
      void* ptr = malloc(sizeof(void*));
      ASSERT_FALSE(ptr == nullptr);
      batch.push_back(ptr);
      pushed.insert(ptr);
    }

    list_.PushBatch(batch_size, batch.data());
    length += batch_size;

    EXPECT_EQ(length, list_.length());
    EXPECT_EQ(length == 0, list_.empty());
  }

  absl::flat_hash_set<void*> popped;
  for (int batch_size : batch_sizes) {
    std::vector<void*> batch(batch_size, nullptr);
    list_.PopBatch(batch_size, batch.data());
    length -= batch_size;

    popped.insert(batch.begin(), batch.end());
    EXPECT_EQ(length, list_.length());
    EXPECT_EQ(length == 0, list_.empty());
  }

  EXPECT_EQ(pushed, popped);

  for (void* ptr : pushed) {
    free(ptr);
  }
}

class TListTest : public ::testing::Test {
 protected:
  MockSpanList list_;

  static_assert(!std::is_copy_constructible<MockSpanList>::value,
                "TList should not be copyable");
  static_assert(!std::is_move_constructible<MockSpanList>::value,
                "TList should not be movable");
};

TEST_F(TListTest, AppendPushPop) {
  const int N = 20;

  EXPECT_EQ(list_.length(), 0);
  EXPECT_TRUE(list_.empty());

  // Append N elements to the list.
  for (int i = 0; i < N; i++) {
    MockSpan* s = MockSpan::New(i);
    ASSERT_FALSE(s == nullptr);
    list_.append(s);
    EXPECT_EQ(list_.first()->index_, 0);
    EXPECT_EQ(list_.last()->index_, i);

    EXPECT_EQ(list_.length(), i + 1);
    EXPECT_FALSE(list_.empty());
  }

  // Remove all N elements from the end of the list.
  for (int i = N; i > 0; i--) {
    EXPECT_EQ(list_.length(), i);
    EXPECT_FALSE(list_.empty());

    MockSpan* last = list_.last();
    EXPECT_EQ(list_.first()->index_, 0);
    EXPECT_EQ(list_.last()->index_, i - 1);

    EXPECT_FALSE(last == nullptr);
    bool ret = list_.remove(last);
    // Returns true iff the list is empty after the remove.
    EXPECT_EQ(ret, i == 1);

    delete last;
  }
  EXPECT_EQ(list_.length(), 0);
  EXPECT_TRUE(list_.empty());
}

TEST_F(TListTest, PrependPushPop) {
  const int N = 20;

  EXPECT_EQ(list_.length(), 0);
  EXPECT_TRUE(list_.empty());

  // Prepend N elements to the list.
  for (int i = 0; i < N; i++) {
    MockSpan* s = MockSpan::New(i);
    ASSERT_FALSE(s == nullptr);
    list_.prepend(s);
    EXPECT_EQ(list_.first()->index_, i);
    EXPECT_EQ(list_.last()->index_, 0);

    EXPECT_EQ(list_.length(), i + 1);
    EXPECT_FALSE(list_.empty());
  }

  // Check range iterator
  {
    int x = N - 1;
    for (const MockSpan* s : list_) {
      EXPECT_EQ(s->index_, x);
      x--;
    }
  }

  // Remove all N elements from the front of the list.
  for (int i = N; i > 0; i--) {
    EXPECT_EQ(list_.length(), i);
    EXPECT_FALSE(list_.empty());

    MockSpan* first = list_.first();
    EXPECT_EQ(list_.first()->index_, i - 1);
    EXPECT_EQ(list_.last()->index_, 0);

    EXPECT_FALSE(first == nullptr);
    bool ret = list_.remove(first);
    // Returns true iff the list is empty after the remove.
    EXPECT_EQ(ret, i == 1);

    delete first;
  }
  EXPECT_EQ(list_.length(), 0);
  EXPECT_TRUE(list_.empty());
}

TEST_F(TListTest, AppendRandomRemove) {
  const int N = 100;
  std::vector<MockSpan*> v(N);

  // Append N elements to the list.
  for (int i = 0; i < N; i++) {
    MockSpan* s = MockSpan::New(i);
    ASSERT_FALSE(s == nullptr);
    v[i] = s;
    list_.append(s);
  }

  // Remove all N elements from the list in a random order
  std::shuffle(v.begin(), v.end(), absl::BitGen());
  int i = N;
  for (MockSpan* s : v) {
    EXPECT_EQ(list_.length(), i);
    EXPECT_FALSE(list_.empty());

    bool ret = list_.remove(s);
    // Returns true iff the list is empty after the remove.
    EXPECT_EQ(ret, i == 1);

    delete s;
    i--;
  }
  EXPECT_EQ(list_.length(), 0);
  EXPECT_TRUE(list_.empty());
}

}  // namespace
}  // namespace tcmalloc_internal
}  // namespace tcmalloc
