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

#include "mongo/unittest/unittest.h"

#include "mongo/db/update_index_data.h"

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
    a.allPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

TEST(UpdateIndexDataTest, AllPathsIndexed2) {
    UpdateIndexData a;
    a.allPathsIndexed();
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("")));
    a.addPathComponent("a"_sd);
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("a")));
    ASSERT_TRUE(a.mightBeIndexed(FieldRef("b")));
    a.clear();
    ASSERT_FALSE(a.mightBeIndexed(FieldRef("a")));
}

TEST(UpdateIndexDataTest, CanonicalIndexField) {
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("aaa")), FieldRef("aaa"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.b")), FieldRef("a.b"_sd));

    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.$")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.123")), FieldRef("a"_sd));

    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.$.b")), FieldRef("a.b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0.b")), FieldRef("a.b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.123.b")), FieldRef("a.b"_sd));

    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.$ref")), FieldRef("a.$ref"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.$ref.b")),
              FieldRef("a.$ref.b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.c$d.b")), FieldRef("a.c$d.b"_sd));

    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.123a")), FieldRef("a.123a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.a123")), FieldRef("a.a123"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.123a.b")),
              FieldRef("a.123a.b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.a123.b")),
              FieldRef("a.a123.b"_sd));

    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.")), FieldRef("a."_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("$")), FieldRef("$"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("$.a")), FieldRef("$.a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.$")), FieldRef("a"_sd));
}

TEST(UpdateIndexDataTest, CanonicalIndexFieldForNestedNumericFieldNames) {
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0.0")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.55.01")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0.0.b.1")), FieldRef("a"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0b.1")), FieldRef("a.0b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.0.b.1.2")), FieldRef("a.b"_sd));
    ASSERT_EQ(UpdateIndexData::getCanonicalIndexField(FieldRef("a.01.02.b.c")), FieldRef("a"_sd));
}
}  // namespace mongo
