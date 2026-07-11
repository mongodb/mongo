// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/column/interleaved_schema.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

using Op = InterleavedSchema::Op;

TEST(InterleavedSchemaTest, FlatObject) {
    auto ref = BSON("a" << 1 << "b" << 2);
    InterleavedSchema schema(ref, BSONType::object, false);

    EXPECT_EQ(schema.scalarCount(), 2);

    auto& e = schema.entries();
    ASSERT_EQ(e.size(), 4);  // Enter, Scalar(a), Scalar(b), Exit

    EXPECT_EQ(e[0].op, Op::kEnterSubObj);
    EXPECT_EQ(e[0].fieldName, ""sv);
    EXPECT_EQ(e[0].type, BSONType::object);

    EXPECT_EQ(e[1].op, Op::kScalar);
    EXPECT_EQ(e[1].fieldName, "a"sv);
    EXPECT_EQ(e[1].stateIdx, 0);

    EXPECT_EQ(e[2].op, Op::kScalar);
    EXPECT_EQ(e[2].fieldName, "b"sv);
    EXPECT_EQ(e[2].stateIdx, 1);

    EXPECT_EQ(e[3].op, Op::kExitSubObj);
}

TEST(InterleavedSchemaTest, NestedObject) {
    auto ref = BSON("a" << 1 << "obj" << BSON("b" << 2 << "c" << 3));
    InterleavedSchema schema(ref, BSONType::object, false);

    EXPECT_EQ(schema.scalarCount(), 3);

    auto& e = schema.entries();
    // Enter(root), Scalar(a,0), Enter(obj), Scalar(b,1), Scalar(c,2), Exit(obj), Exit(root)
    ASSERT_EQ(e.size(), 7);

    EXPECT_EQ(e[0].op, Op::kEnterSubObj);
    EXPECT_EQ(e[1].op, Op::kScalar);
    EXPECT_EQ(e[1].fieldName, "a"sv);
    EXPECT_EQ(e[1].stateIdx, 0);

    EXPECT_EQ(e[2].op, Op::kEnterSubObj);
    EXPECT_EQ(e[2].fieldName, "obj"sv);
    EXPECT_EQ(e[2].type, BSONType::object);

    EXPECT_EQ(e[3].op, Op::kScalar);
    EXPECT_EQ(e[3].stateIdx, 1);
    EXPECT_EQ(e[4].op, Op::kScalar);
    EXPECT_EQ(e[4].stateIdx, 2);

    EXPECT_EQ(e[5].op, Op::kExitSubObj);
    EXPECT_EQ(e[5].fieldName, "obj"sv);
    EXPECT_EQ(e[6].op, Op::kExitSubObj);
    EXPECT_EQ(e[6].fieldName, ""sv);
}

TEST(InterleavedSchemaTest, DeepNesting) {
    auto ref = BSON("a" << BSON("b" << BSON("c" << BSON("d" << 1)) << "e" << 2) << "f" << 3);
    InterleavedSchema schema(ref, BSONType::object, false);

    EXPECT_EQ(schema.scalarCount(), 3);

    // Verify stateIdx is sequential across depth: d=0, e=1, f=2
    auto& e = schema.entries();
    int scalarsSeen = 0;
    for (auto& entry : e) {
        if (entry.op == Op::kScalar) {
            EXPECT_EQ(entry.stateIdx, scalarsSeen);
            scalarsSeen++;
        }
    }
    EXPECT_EQ(scalarsSeen, 3);
}

TEST(InterleavedSchemaTest, EmptySubObject) {
    auto ref = BSON("obj" << BSONObj());
    InterleavedSchema schema(ref, BSONType::object, false);

    EXPECT_EQ(schema.scalarCount(), 0);

    auto& e = schema.entries();
    // Enter(root), Enter(obj), Exit(obj), Exit(root)
    ASSERT_EQ(e.size(), 4);

    EXPECT_EQ(e[1].op, Op::kEnterSubObj);
    EXPECT_EQ(e[1].fieldName, "obj"sv);
    EXPECT_TRUE(e[1].allowEmpty);

    EXPECT_EQ(e[2].op, Op::kExitSubObj);
    EXPECT_TRUE(e[2].allowEmpty);
}

TEST(InterleavedSchemaTest, EmptyRootObject) {
    InterleavedSchema schema(BSONObj(), BSONType::object, false);

    EXPECT_EQ(schema.scalarCount(), 0);

    auto& e = schema.entries();
    ASSERT_EQ(e.size(), 2);  // Enter + Exit
    EXPECT_EQ(e[0].op, Op::kEnterSubObj);
    EXPECT_TRUE(e[0].allowEmpty);
    EXPECT_EQ(e[1].op, Op::kExitSubObj);
}

TEST(InterleavedSchemaTest, ArraysFlagTrue) {
    auto ref = BSON("a" << 1 << "arr" << BSON_ARRAY(10 << 20));
    InterleavedSchema schema(ref, BSONType::object, true);

    // With arrays=true, the array should be treated as a sub-object
    auto& e = schema.entries();
    bool foundArrayEnter = false;
    for (auto& entry : e) {
        if (entry.fieldName == "arr"sv && entry.op == Op::kEnterSubObj) {
            foundArrayEnter = true;
            EXPECT_EQ(entry.type, BSONType::array);
        }
    }
    EXPECT_TRUE(foundArrayEnter);
}

TEST(InterleavedSchemaTest, ArraysFlagFalse) {
    auto ref = BSON("a" << 1 << "arr" << BSON_ARRAY(10 << 20));
    InterleavedSchema schema(ref, BSONType::object, false);

    // With arrays=false, the array should be treated as a scalar
    auto& e = schema.entries();
    bool foundArrayScalar = false;
    for (auto& entry : e) {
        if (entry.fieldName == "arr"sv) {
            EXPECT_EQ(entry.op, Op::kScalar);
            foundArrayScalar = true;
        }
    }
    EXPECT_TRUE(foundArrayScalar);
}

TEST(InterleavedSchemaTest, RootTypeArray) {
    auto ref = BSON("a" << 1);
    InterleavedSchema schema(ref, BSONType::array, false);

    auto& e = schema.entries();
    EXPECT_EQ(e[0].op, Op::kEnterSubObj);
    EXPECT_EQ(e[0].type, BSONType::array);
}

TEST(InterleavedSchemaTest, ScalarCountAtBsonMaxElements) {
    // 1 (type) + key digits + 1 (null) + 4 (int32)
    auto estimateSize = [](int i) -> int {
        return 6 + static_cast<int>(std::to_string(i).size());
    };

    // 4 (outer size) + 1 (type) + 5 ("data\0") + 4 (array size) + 1 (array null) + 1 (outer null)
    constexpr int kOuterOverhead = 16;
    size_t totalSize = kOuterOverhead;
    BSONArrayBuilder bab;
    for (int i = 0, size; totalSize + (size = estimateSize(i)) < BSONObjMaxUserSize; ++i) {
        totalSize += size;
        bab.append(i);
    }
    auto arr = bab.arr();
    auto ref = BSON("data" << arr);
    ASSERT_EQ(ref.objsize(), totalSize);

    InterleavedSchema schema(ref, BSONType::object, true);
    EXPECT_EQ(schema.scalarCount(), arr.nFields());
}

}  // namespace
}  // namespace mongo
