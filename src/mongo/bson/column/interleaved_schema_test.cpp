/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/column/interleaved_schema.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using Op = InterleavedSchema::Op;

TEST(InterleavedSchemaTest, FlatObject) {
    auto ref = BSON("a" << 1 << "b" << 2);
    InterleavedSchema schema(ref, BSONType::object, false);

    ASSERT_EQ(schema.scalarCount(), 2);

    auto& e = schema.entries();
    ASSERT_EQ(e.size(), 4);  // Enter, Scalar(a), Scalar(b), Exit

    ASSERT_EQ(e[0].op, Op::kEnterSubObj);
    ASSERT_EQ(e[0].fieldName, ""_sd);
    ASSERT_EQ(e[0].type, BSONType::object);

    ASSERT_EQ(e[1].op, Op::kScalar);
    ASSERT_EQ(e[1].fieldName, "a"_sd);
    ASSERT_EQ(e[1].stateIdx, 0);

    ASSERT_EQ(e[2].op, Op::kScalar);
    ASSERT_EQ(e[2].fieldName, "b"_sd);
    ASSERT_EQ(e[2].stateIdx, 1);

    ASSERT_EQ(e[3].op, Op::kExitSubObj);
}

TEST(InterleavedSchemaTest, NestedObject) {
    auto ref = BSON("a" << 1 << "obj" << BSON("b" << 2 << "c" << 3));
    InterleavedSchema schema(ref, BSONType::object, false);

    ASSERT_EQ(schema.scalarCount(), 3);

    auto& e = schema.entries();
    // Enter(root), Scalar(a,0), Enter(obj), Scalar(b,1), Scalar(c,2), Exit(obj), Exit(root)
    ASSERT_EQ(e.size(), 7);

    ASSERT_EQ(e[0].op, Op::kEnterSubObj);
    ASSERT_EQ(e[1].op, Op::kScalar);
    ASSERT_EQ(e[1].fieldName, "a"_sd);
    ASSERT_EQ(e[1].stateIdx, 0);

    ASSERT_EQ(e[2].op, Op::kEnterSubObj);
    ASSERT_EQ(e[2].fieldName, "obj"_sd);
    ASSERT_EQ(e[2].type, BSONType::object);

    ASSERT_EQ(e[3].op, Op::kScalar);
    ASSERT_EQ(e[3].stateIdx, 1);
    ASSERT_EQ(e[4].op, Op::kScalar);
    ASSERT_EQ(e[4].stateIdx, 2);

    ASSERT_EQ(e[5].op, Op::kExitSubObj);
    ASSERT_EQ(e[5].fieldName, "obj"_sd);
    ASSERT_EQ(e[6].op, Op::kExitSubObj);
    ASSERT_EQ(e[6].fieldName, ""_sd);
}

TEST(InterleavedSchemaTest, DeepNesting) {
    auto ref = BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)) << "e" << 2) << "f" << 3);
    InterleavedSchema schema(ref, BSONType::object, false);

    ASSERT_EQ(schema.scalarCount(), 3);

    // Verify stateIdx is sequential across depth: d=0, e=1, f=2
    auto& e = schema.entries();
    int scalarsSeen = 0;
    for (auto& entry : e) {
        if (entry.op == Op::kScalar) {
            ASSERT_EQ(entry.stateIdx, scalarsSeen);
            scalarsSeen++;
        }
    }
    ASSERT_EQ(scalarsSeen, 3);
}

TEST(InterleavedSchemaTest, EmptySubObject) {
    auto ref = BSON("obj" << BSONObj());
    InterleavedSchema schema(ref, BSONType::object, false);

    ASSERT_EQ(schema.scalarCount(), 0);

    auto& e = schema.entries();
    // Enter(root), Enter(obj), Exit(obj), Exit(root)
    ASSERT_EQ(e.size(), 4);

    ASSERT_EQ(e[1].op, Op::kEnterSubObj);
    ASSERT_EQ(e[1].fieldName, "obj"_sd);
    ASSERT_TRUE(e[1].allowEmpty);

    ASSERT_EQ(e[2].op, Op::kExitSubObj);
    ASSERT_TRUE(e[2].allowEmpty);
}

TEST(InterleavedSchemaTest, EmptyRootObject) {
    InterleavedSchema schema(BSONObj(), BSONType::object, false);

    ASSERT_EQ(schema.scalarCount(), 0);

    auto& e = schema.entries();
    ASSERT_EQ(e.size(), 2);  // Enter + Exit
    ASSERT_EQ(e[0].op, Op::kEnterSubObj);
    ASSERT_TRUE(e[0].allowEmpty);
    ASSERT_EQ(e[1].op, Op::kExitSubObj);
}

TEST(InterleavedSchemaTest, ArraysFlagTrue) {
    auto ref = BSON("a" << 1 << "arr" << BSON_ARRAY(10 << 20));
    InterleavedSchema schema(ref, BSONType::object, true);

    // With arrays=true, the array should be treated as a sub-object
    auto& e = schema.entries();
    bool foundArrayEnter = false;
    for (auto& entry : e) {
        if (entry.fieldName == "arr"_sd && entry.op == Op::kEnterSubObj) {
            foundArrayEnter = true;
            ASSERT_EQ(entry.type, BSONType::array);
        }
    }
    ASSERT_TRUE(foundArrayEnter);
}

TEST(InterleavedSchemaTest, ArraysFlagFalse) {
    auto ref = BSON("a" << 1 << "arr" << BSON_ARRAY(10 << 20));
    InterleavedSchema schema(ref, BSONType::object, false);

    // With arrays=false, the array should be treated as a scalar
    auto& e = schema.entries();
    bool foundArrayScalar = false;
    for (auto& entry : e) {
        if (entry.fieldName == "arr"_sd) {
            ASSERT_EQ(entry.op, Op::kScalar);
            foundArrayScalar = true;
        }
    }
    ASSERT_TRUE(foundArrayScalar);
}

TEST(InterleavedSchemaTest, RootTypeArray) {
    auto ref = BSON("a" << 1);
    InterleavedSchema schema(ref, BSONType::array, false);

    auto& e = schema.entries();
    ASSERT_EQ(e[0].op, Op::kEnterSubObj);
    ASSERT_EQ(e[0].type, BSONType::array);
}

}  // namespace
}  // namespace mongo
