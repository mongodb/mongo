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

#include "absl/strings/internal/cord_internal.h"

#include "gmock/gmock.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

TEST(RefcountAndFlags, NormalRefcount) {
  for (bool expect_high_refcount : {false, true}) {
    SCOPED_TRACE(expect_high_refcount);
    RefcountAndFlags refcount;
    // count = 1

    EXPECT_FALSE(refcount.HasCrc());
    EXPECT_TRUE(refcount.IsMutable());
    EXPECT_TRUE(refcount.IsOne());

    refcount.Increment();
    // count = 2

    EXPECT_FALSE(refcount.HasCrc());
    EXPECT_FALSE(refcount.IsMutable());
    EXPECT_FALSE(refcount.IsOne());

    // Decrementing should return true, since a reference is outstanding.
    if (expect_high_refcount) {
      EXPECT_TRUE(refcount.DecrementExpectHighRefcount());
    } else {
      EXPECT_TRUE(refcount.Decrement());
    }
    // count = 1

    EXPECT_FALSE(refcount.HasCrc());
    EXPECT_TRUE(refcount.IsMutable());
    EXPECT_TRUE(refcount.IsOne());

    // One more decremnt will return false, as no references remain.
    if (expect_high_refcount) {
      EXPECT_FALSE(refcount.DecrementExpectHighRefcount());
    } else {
      EXPECT_FALSE(refcount.Decrement());
    }
  }
}

TEST(RefcountAndFlags, CrcRefcount) {
  for (bool expect_high_refcount : {false, true}) {
    SCOPED_TRACE(expect_high_refcount);
    RefcountAndFlags refcount(RefcountAndFlags::WithCrc{});
    // count = 1

    // A CRC-carrying node is never mutable, but can be unshared
    EXPECT_TRUE(refcount.HasCrc());
    EXPECT_FALSE(refcount.IsMutable());
    EXPECT_TRUE(refcount.IsOne());

    refcount.Increment();
    // count = 2

    EXPECT_TRUE(refcount.HasCrc());
    EXPECT_FALSE(refcount.IsMutable());
    EXPECT_FALSE(refcount.IsOne());

    // Decrementing should return true, since a reference is outstanding.
    if (expect_high_refcount) {
      EXPECT_TRUE(refcount.DecrementExpectHighRefcount());
    } else {
      EXPECT_TRUE(refcount.Decrement());
    }
    // count = 1

    EXPECT_TRUE(refcount.HasCrc());
    EXPECT_FALSE(refcount.IsMutable());
    EXPECT_TRUE(refcount.IsOne());

    // One more decremnt will return false, as no references remain.
    if (expect_high_refcount) {
      EXPECT_FALSE(refcount.DecrementExpectHighRefcount());
    } else {
      EXPECT_FALSE(refcount.Decrement());
    }
  }
}

TEST(RefcountAndFlags, ImmortalRefcount) {
  RefcountAndFlags immortal_refcount(RefcountAndFlags::Immortal{});

  for (int i = 0; i < 100; ++i) {
    // An immortal refcount is never unshared, and decrementing never causes
    // a collection.
    EXPECT_FALSE(immortal_refcount.HasCrc());
    EXPECT_FALSE(immortal_refcount.IsMutable());
    EXPECT_FALSE(immortal_refcount.IsOne());
    EXPECT_TRUE(immortal_refcount.Decrement());
    EXPECT_TRUE(immortal_refcount.DecrementExpectHighRefcount());
  }
}

}  // namespace cord_internal
ABSL_NAMESPACE_END
}  // namespace absl
