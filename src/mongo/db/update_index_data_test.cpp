// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update_index_data.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
using namespace std::literals::string_view_literals;

TEST(UpdateIndexDataTest, Simple1) {
    UpdateIndexData a;
    a.addPath(FieldRef("a.b"sv));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a.b")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a.b.c")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a.$.b")));

    ASSERT_FALSE(a.mightBeIndexed(FieldRef("b")));
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a.c")));

    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a.b")));
}

TEST(UpdateIndexDataTest, Simple2) {
    UpdateIndexData a;
    a.addPath(FieldRef("ab"sv));
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("ab")));
}

TEST(UpdateIndexDataTest, Component1) {
    UpdateIndexData a;
    a.addPathComponent("a"sv);
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("b.a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a.b")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("b.a.c")));
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("b.c")));
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("ab")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

TEST(UpdateIndexDataTest, AllPathsIndexed1) {
    UpdateIndexData a;
    a.setAllPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

TEST(UpdateIndexDataTest, AllPathsIndexed2) {
    UpdateIndexData a;
    a.setAllPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("")));
    a.addPathComponent("a"sv);
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("b")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

}  // namespace mongo
