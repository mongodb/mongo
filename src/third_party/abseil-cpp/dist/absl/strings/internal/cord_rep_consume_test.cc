// Copyright 2021 The Abseil Authors
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

#include "absl/strings/internal/cord_rep_consume.h"

#include <functional>
#include <utility>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/strings/internal/cord_internal.h"
#include "absl/strings/internal/cord_rep_flat.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {
namespace {

using testing::InSequence;
using testing::MockFunction;

// Returns the depth of a node
int Depth(const CordRep* rep) {
  return (rep->tag == CONCAT) ? rep->concat()->depth() : 0;
}

// Creates a concatenation of the specified nodes.
CordRepConcat* CreateConcat(CordRep* left, CordRep* right) {
  auto* concat = new CordRepConcat();
  concat->tag = CONCAT;
  concat->left = left;
  concat->right = right;
  concat->length = left->length + right->length;
  concat->set_depth(1 + (std::max)(Depth(left), Depth(right)));
  return concat;
}

// Creates a flat with the length set to `length`
CordRepFlat* CreateFlatWithLength(size_t length) {
  auto* flat = CordRepFlat::New(length);
  flat->length = length;
  return flat;
}

// Creates a substring node on the specified child.
CordRepSubstring* CreateSubstring(CordRep* child, size_t start, size_t length) {
  auto* rep = new CordRepSubstring();
  rep->length = length;
  rep->tag = SUBSTRING;
  rep->start = start;
  rep->child = child;
  return rep;
}

// Flats we use in the tests
CordRep* flat[6];

// Creates a test tree
CordRep* CreateTestTree() {
  flat[0] = CreateFlatWithLength(1);
  flat[1] = CreateFlatWithLength(7);
  CordRepConcat* left = CreateConcat(flat[0], CreateSubstring(flat[1], 2, 4));

  flat[2] = CreateFlatWithLength(9);
  flat[3] = CreateFlatWithLength(13);
  CordRepConcat* right1 = CreateConcat(flat[2], flat[3]);

  flat[4] = CreateFlatWithLength(15);
  flat[5] = CreateFlatWithLength(19);
  CordRepConcat* right2 = CreateConcat(flat[4], flat[5]);

  CordRepConcat* right = CreateConcat(right1, CreateSubstring(right2, 5, 17));
  return CreateConcat(left, right);
}

TEST(CordRepConsumeTest, Consume) {
  InSequence in_sequence;
  CordRep* tree = CreateTestTree();
  MockFunction<void(CordRep*, size_t, size_t)> consume;
  EXPECT_CALL(consume, Call(flat[0], 0, 1));
  EXPECT_CALL(consume, Call(flat[1], 2, 4));
  EXPECT_CALL(consume, Call(flat[2], 0, 9));
  EXPECT_CALL(consume, Call(flat[3], 0, 13));
  EXPECT_CALL(consume, Call(flat[4], 5, 10));
  EXPECT_CALL(consume, Call(flat[5], 0, 7));
  Consume(tree, consume.AsStdFunction());
  for (CordRep* rep : flat) {
    EXPECT_TRUE(rep->refcount.IsOne());
    CordRep::Unref(rep);
  }
}

TEST(CordRepConsumeTest, ConsumeShared) {
  InSequence in_sequence;
  CordRep* tree = CreateTestTree();
  MockFunction<void(CordRep*, size_t, size_t)> consume;
  EXPECT_CALL(consume, Call(flat[0], 0, 1));
  EXPECT_CALL(consume, Call(flat[1], 2, 4));
  EXPECT_CALL(consume, Call(flat[2], 0, 9));
  EXPECT_CALL(consume, Call(flat[3], 0, 13));
  EXPECT_CALL(consume, Call(flat[4], 5, 10));
  EXPECT_CALL(consume, Call(flat[5], 0, 7));
  Consume(CordRep::Ref(tree), consume.AsStdFunction());
  for (CordRep* rep : flat) {
    EXPECT_FALSE(rep->refcount.IsOne());
    CordRep::Unref(rep);
  }
  CordRep::Unref(tree);
}

TEST(CordRepConsumeTest, Reverse) {
  InSequence in_sequence;
  CordRep* tree = CreateTestTree();
  MockFunction<void(CordRep*, size_t, size_t)> consume;
  EXPECT_CALL(consume, Call(flat[5], 0, 7));
  EXPECT_CALL(consume, Call(flat[4], 5, 10));
  EXPECT_CALL(consume, Call(flat[3], 0, 13));
  EXPECT_CALL(consume, Call(flat[2], 0, 9));
  EXPECT_CALL(consume, Call(flat[1], 2, 4));
  EXPECT_CALL(consume, Call(flat[0], 0, 1));
  ReverseConsume(tree, consume.AsStdFunction());
  for (CordRep* rep : flat) {
    EXPECT_TRUE(rep->refcount.IsOne());
    CordRep::Unref(rep);
  }
}

TEST(CordRepConsumeTest, ReverseShared) {
  InSequence in_sequence;
  CordRep* tree = CreateTestTree();
  MockFunction<void(CordRep*, size_t, size_t)> consume;
  EXPECT_CALL(consume, Call(flat[5], 0, 7));
  EXPECT_CALL(consume, Call(flat[4], 5, 10));
  EXPECT_CALL(consume, Call(flat[3], 0, 13));
  EXPECT_CALL(consume, Call(flat[2], 0, 9));
  EXPECT_CALL(consume, Call(flat[1], 2, 4));
  EXPECT_CALL(consume, Call(flat[0], 0, 1));
  ReverseConsume(CordRep::Ref(tree), consume.AsStdFunction());
  for (CordRep* rep : flat) {
    EXPECT_FALSE(rep->refcount.IsOne());
    CordRep::Unref(rep);
  }
  CordRep::Unref(tree);
}

TEST(CordRepConsumeTest, UnreachableFlat) {
  InSequence in_sequence;
  CordRepFlat* flat1 = CreateFlatWithLength(10);
  CordRepFlat* flat2 = CreateFlatWithLength(20);
  CordRepConcat* concat = CreateConcat(flat1, flat2);
  CordRepSubstring* tree = CreateSubstring(concat, 15, 10);
  MockFunction<void(CordRep*, size_t, size_t)> consume;
  EXPECT_CALL(consume, Call(flat2, 5, 10));
  Consume(tree, consume.AsStdFunction());
  EXPECT_TRUE(flat2->refcount.IsOne());
  CordRep::Unref(flat2);
}

}  // namespace
}  // namespace cord_internal
ABSL_NAMESPACE_END
}  // namespace absl
