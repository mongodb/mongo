/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/update_index_data.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(UpdateIndexDataTest, Simple1) {
    UpdateIndexData a;
    a.addPath(FieldRef("a.b"_sd));
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
    a.addPath(FieldRef("ab"_sd));
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("ab")));
}

TEST(UpdateIndexDataTest, Component1) {
    UpdateIndexData a;
    a.addPathComponent("a"_sd);
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
    a.addPathComponent("a"_sd);
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("b")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

}  // namespace mongo
