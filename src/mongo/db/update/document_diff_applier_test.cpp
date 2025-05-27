/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/update/document_diff_applier.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/document_diff_test_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::doc_diff {
namespace {
/**
 * Checks that applying the diff (once or twice) to 'preImage' produces the expected post image.
 */
void checkDiff(const BSONObj& preImage, const BSONObj& expectedPost, const Diff& diff) {
    BSONObj postImage = applyDiffTestHelper(preImage, diff);

    // This *MUST* check for binary equality, which is what we enforce between replica set
    // members. Logical equality (through woCompare() or ASSERT_BSONOBJ_EQ) is not enough to show
    // that the applier actually works.
    ASSERT_BSONOBJ_BINARY_EQ(postImage, expectedPost);

    BSONObj postImageAgain = applyDiffTestHelper(postImage, diff);
    ASSERT_BSONOBJ_BINARY_EQ(postImageAgain, expectedPost);
}

TEST(DiffApplierTest, DeleteSimple) {
    const BSONObj preImage(BSON("f1" << 1 << "foo" << 2 << "f2" << 3));

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addDelete("f1");
    diffNode.addDelete("f2");
    diffNode.addDelete("f3");

    auto diff = diffNode.serialize();

    checkDiff(preImage, BSON("foo" << 2), diff);
}

TEST(DiffApplierTest, InsertSimple) {
    const BSONObj preImage(BSON("f1" << 1 << "foo" << 2 << "f2" << 3));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData newField = "newField";

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addInsert("f1", storage["a"]);
    diffNode.addInsert(newField, storage["b"]);

    auto diff = diffNode.serialize();
    checkDiff(preImage, BSON("foo" << 2 << "f2" << 3 << "f1" << 1 << "newField" << 2), diff);
}

TEST(DiffApplierTest, BinaryAppendSimple) {
    // Appends "def" to "abc" for BSONColumn.
    const BSONObj preImage(BSON("b1" << BSONBinData("abc", 3, BinDataType::Column)));

    const BSONObj storage(
        BSON("a" << BSON("o" << 3 << "d" << BSONBinData("def", 3, BinDataType::BinDataGeneral))));

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addBinary("b1", storage["a"]);

    auto diff = diffNode.serialize();
    checkDiff(preImage, BSON("b1" << BSONBinData("abcdef", 6, BinDataType::Column)), diff);
}

TEST(DiffApplierTest, BinaryOverwriteSimple) {
    // Replaces "abc" with "abdef".
    const BSONObj preImage(BSON("b1" << BSONBinData("abc", 3, BinDataType::Column)));

    const BSONObj storage(
        BSON("a" << BSON("o" << 2 << "d" << BSONBinData("def", 3, BinDataType::BinDataGeneral))));

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addBinary("b1", storage["a"]);

    auto diff = diffNode.serialize();
    checkDiff(preImage, BSON("b1" << BSONBinData("abdef", 5, BinDataType::Column)), diff);
}

TEST(DiffApplierTest, BinaryIsIdempotent) {
    // Applies "def" to "abc" twice using the same diff. The final result is expected to be
    // "abcdef".
    auto applyDiffToImage = [&](BSONObj preImage) -> BSONObj {
        const BSONObj storage(BSON(
            "a" << BSON("o" << 3 << "d" << BSONBinData("def", 3, BinDataType::BinDataGeneral))));

        diff_tree::DocumentSubDiffNode diffNode;
        diffNode.addBinary("b1", storage["a"]);

        return diffNode.serialize();
    };

    const BSONObj firstImage(BSON("b1" << BSONBinData("abc", 3, BinDataType::Column)));

    auto secondImage = applyDiffToImage(firstImage);
    checkDiff(firstImage, BSON("b1" << BSONBinData("abcdef", 6, BinDataType::Column)), secondImage);

    auto thirdImage = applyDiffToImage(secondImage);
    checkDiff(firstImage, BSON("b1" << BSONBinData("abcdef", 6, BinDataType::Column)), thirdImage);
}

TEST(DiffApplierTest, BinaryIsIdempotentMultipleWithShrink) {
    // Applies "def" to "abc" and then shinks to "ax" twice using the same two diffs. The final
    // result is expected to be "ax".
    auto diffAppend = [&]() -> BSONObj {
        const BSONObj storage(BSON(
            "a" << BSON("o" << 3 << "d" << BSONBinData("def", 3, BinDataType::BinDataGeneral))));

        diff_tree::DocumentSubDiffNode diffNode;
        diffNode.addBinary("b1", storage["a"]);

        return diffNode.serialize();
    };

    auto diffShrink = [&]() -> BSONObj {
        const BSONObj storage(
            BSON("a" << BSON("o" << 1 << "d" << BSONBinData("x", 1, BinDataType::BinDataGeneral))));

        diff_tree::DocumentSubDiffNode diffNode;
        diffNode.addBinary("b1", storage["a"]);

        return diffNode.serialize();
    };

    const BSONObj firstImage(BSON("b1" << BSONBinData("abc", 3, BinDataType::Column)));
    const BSONObj secondImage(BSON("b1" << BSONBinData("abcdef", 6, BinDataType::Column)));
    const BSONObj thirdImage(BSON("b1" << BSONBinData("ax", 2, BinDataType::Column)));

    checkDiff(firstImage, secondImage, diffAppend());
    checkDiff(secondImage, thirdImage, diffShrink());

    checkDiff(thirdImage, thirdImage, diffAppend());
    checkDiff(thirdImage, thirdImage, diffShrink());
}

TEST(DiffApplierTest, UpdateSimple) {
    const BSONObj preImage(BSON("f1" << 0 << "foo" << 2 << "f2" << 3));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData newField = "newField";

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addUpdate("f1", storage["a"]);
    diffNode.addUpdate(newField, storage["b"]);

    auto diff = diffNode.serialize();
    checkDiff(preImage, BSON("f1" << 1 << "foo" << 2 << "f2" << 3 << "newField" << 2), diff);
}

TEST(DiffApplierTest, SubObjDiffSimple) {
    const BSONObj preImage(
        BSON("obj" << BSON("dField" << 0 << "iField" << 0 << "uField" << 0) << "otherField" << 0));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subDiffNode->addDelete("dField");
        subDiffNode->addInsert("iField", storage["a"]);
        subDiffNode->addUpdate("uField", storage["b"]);
        diffNode.addChild("obj", std::move(subDiffNode));
    }

    auto diff = diffNode.serialize();
    checkDiff(
        preImage, BSON("obj" << BSON("uField" << 2 << "iField" << 1) << "otherField" << 0), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithAppend) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(999 << 999 << 999 << 999)));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData arr = "arr";

    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(1, storage["a"]);
        subDiffNode->addUpdate(4, storage["b"]);
        diffNode.addChild(arr, std::move(subDiffNode));
    }

    auto diff = diffNode.serialize();

    checkDiff(preImage, BSON("arr" << BSON_ARRAY(999 << 1 << 999 << 999 << 2)), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithTruncate) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(999 << 999 << 999 << 999)));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData arr = "arr";

    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(1, storage["a"]);
        subDiffNode->setResize(3);
        diffNode.addChild(arr, std::move(subDiffNode));
    }

    auto diff = diffNode.serialize();
    checkDiff(preImage, BSON("arr" << BSON_ARRAY(999 << 1 << 999)), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithNullPadding) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(0)));

    BSONObj storage(BSON("a" << 1));
    StringData arr = "arr";

    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(3, storage["a"]);
        diffNode.addChild(arr, std::move(subDiffNode));
    }

    auto diff = diffNode.serialize();

    checkDiff(preImage, BSON("arr" << BSON_ARRAY(0 << NullLabeler{} << NullLabeler{} << 1)), diff);
}

TEST(DiffApplierTest, NestedSubObjUpdateScalar) {
    BSONObj storage(BSON("a" << 1));
    StringData subObj = "subObj";
    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        {
            auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();

            subSubDiffNode->addUpdate("a", storage["a"]);
            subDiffNode->addChild(subObj, std::move(subSubDiffNode));
        }
        diffNode.addChild(subObj, std::move(subDiffNode));
    }
    auto diff = diffNode.serialize();

    // Check the case where the object matches the structure we expect.
    BSONObj preImage(fromjson("{subObj: {subObj: {a: 0}}}"));
    checkDiff(preImage, fromjson("{subObj: {subObj: {a: 1}}}"), diff);

    // Check cases where the object does not match the structure we expect.
    preImage = BSON("someOtherField" << 1);
    checkDiff(preImage,
              preImage,  // In this case the diff is a no-op.
              diff);

    preImage = BSON("dummyA" << 1 << "dummyB" << 2 << "subObj"
                             << "scalar!"
                             << "dummyC" << 3 << "dummyD" << 4);
    checkDiff(
        preImage, fromjson("{dummyA: 1, dummyB: 2, subObj: null, dummyC: 3, dummyD: 4}"), diff);

    preImage = BSON("dummyA" << 1 << "dummyB" << 2 << "subObj"
                             << BSON("subDummyA" << 1 << "subObj"
                                                 << "scalar!"
                                                 << "subDummyB" << 2)
                             << "dummyC" << 3);
    checkDiff(preImage,
              fromjson("{dummyA: 1, dummyB: 2, "
                       "subObj: {subDummyA: 1, subObj: null, subDummyB: 2}, dummyC: 3}"),
              diff);
}

// Case where the diff contains sub diffs for several array indices.
TEST(DiffApplierTest, UpdateArrayOfObjectsSubDiff) {

    BSONObj storage(
        BSON("uFieldNew" << 999 << "newObj" << BSON("x" << 1) << "a" << 1 << "b" << 2 << "c" << 3));
    StringData arr = "arr";
    StringData dFieldA = "dFieldA";
    StringData dFieldB = "dFieldB";
    StringData uField = "uField";

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addDelete(dFieldA);
    diffNode.addDelete(dFieldB);
    diffNode.addUpdate(uField, storage["uFieldNew"]);

    auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
    {
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subSubDiffNode->addUpdate("a", storage["a"]);
        subDiffNode->addChild(1, std::move(subSubDiffNode));
    }
    {
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subSubDiffNode->addUpdate("b", storage["b"]);
        subDiffNode->addChild(2, std::move(subSubDiffNode));
    }
    {
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subSubDiffNode->addUpdate("c", storage["c"]);
        subDiffNode->addChild(3, std::move(subSubDiffNode));
    }
    subDiffNode->addUpdate(4, storage["newObj"]);
    diffNode.addChild(arr, std::move(subDiffNode));

    auto diff = diffNode.serialize();

    // Case where the object matches the structure we expect.
    BSONObj preImage(
        fromjson("{uField: 1, dFieldA: 1, arr: [null, {a:0}, {b:0}, {c:0}], "
                 "dFieldB: 1}"));
    checkDiff(preImage, fromjson("{uField: 999, arr: [null, {a:1}, {b:2}, {c:3}, {x: 1}]}"), diff);

    preImage = fromjson("{uField: 1, dFieldA: 1, arr: [{a:0}, {b:0}, {c:0}, {}], dFieldB: 1}");
    checkDiff(preImage,
              fromjson("{uField: 999, arr: [{a: 0}, {b:0, a:1}, {c:0, b:2}, {c:3}, {x: 1}]}"),
              diff);

    // Case where the diff is a noop.
    preImage = fromjson("{arr: [null, {a:1}, {b:2}, {c:3}, {x: 1}], uField: 999}");
    checkDiff(preImage, preImage, diff);

    // Case where the pre image has scalars in the array instead of objects. In this case we set
    // some of the indices to null, expecting some future operation to overwrite them. Indexes with
    // an explicit 'update' operation will be overwritten.
    preImage = fromjson("{arr: [0,0,0,0,{x: 2}], uField: 1}");
    checkDiff(preImage, fromjson("{arr: [0, null, null, null, {x: 1}], uField: 999}"), diff);

    // Case where the pre image array is too short. Since the diff contains an array of object sub
    // diffs, the output will be all nulls, which will presumably be overwritten by a future
    // operation.
    preImage = fromjson("{arr: [], uField: 1}");
    checkDiff(preImage, fromjson("{arr: [null, null, null, null, {x: 1}], uField: 999}"), diff);

    // Case where the pre image array is longer than the (presumed) post image.
    preImage = fromjson("{arr: [0,{},{},{},4,5,6], uField: 1}");
    checkDiff(
        preImage, fromjson("{arr: [0, {a:1}, {b:2}, {c:3}, {x:1}, 5, 6], uField: 999}"), diff);

    // Case where the pre image 'arr' field is an object instead of an array. In this
    // situation, we know that some later operation will overwrite the field, so we set it to null
    // ensuring that it keeps its position in the document.
    preImage = fromjson("{dummyA: 1, arr: {foo: 1}, dummyB: 1}");
    checkDiff(preImage, fromjson("{dummyA: 1, arr: null, dummyB: 1, uField: 999}"), diff);

    // Case where pre image 'arr' field is a scalar. Again, we set it to null.
    preImage = fromjson("{dummyA: 1, arr: 1, dummyB: 1}");
    checkDiff(preImage, fromjson("{dummyA: 1, arr: null, dummyB: 1, uField: 999}"), diff);
}

// Case where an array diff rewrites several non contiguous indices which are objects.
TEST(DiffApplierTest, UpdateArrayOfObjectsWithUpdateOperationNonContiguous) {
    BSONObj storage(BSON("dummyA" << 997 << "dummyB" << BSON("newVal" << 998) << "dummyC" << 999));
    StringData arr = "arr";
    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(1, storage["dummyA"]);
        subDiffNode->addUpdate(2, storage["dummyB"]);
        subDiffNode->addUpdate(5, storage["dummyC"]);
        diffNode.addChild(arr, std::move(subDiffNode));
    }

    auto diff = diffNode.serialize();

    // Case where the object matches the structure we expect.
    BSONObj preImage(fromjson("{arr: [null, {}, {}, {}, {}, {}]}"));
    checkDiff(preImage, fromjson("{arr: [null, 997, {newVal: 998}, {}, {}, 999]}"), diff);

    // Case where null padding is necessary before the last element.
    preImage = fromjson("{arr: [{}, {}, {}, {}]}");
    checkDiff(preImage, fromjson("{arr: [{}, 997, {newVal: 998}, {}, null, 999]}"), diff);

    // Case where the diff is a noop.
    preImage = fromjson("{arr: [{}, 997, {newVal: 998}, {}, {}, 999]}");
    checkDiff(preImage, preImage, diff);

    // Case where the pre image array is longer than the (presumed) post image.
    preImage = fromjson("{arr: [0,1,2,3,4,5,6]}");
    checkDiff(preImage, fromjson("{arr: [0, 997, {newVal: 998}, 3, 4, 999, 6]}"), diff);

    // Case where the pre image array contains sub-arrays. The sub-arrays will get overwritten.
    preImage = fromjson("{arr: [0, 1, [], [], ['a', 'b'], 5, 6]}");
    checkDiff(preImage, fromjson("{arr: [0, 997, {newVal: 998}, [], ['a', 'b'], 999, 6]}"), diff);

    // Case where the pre image array contains objects. The objects will be replaced
    preImage = fromjson("{arr: [0, {x:1}, 2, 3, {x:1}, 5, 6]}");
    checkDiff(preImage, fromjson("{arr: [0, 997, {newVal: 998}, 3, {x:1}, 999, 6]}"), diff);

    // Case where 'arr' field is an object. The type mismatch implies that a future operation will
    // re-write the field, so it is set to null.
    preImage = fromjson("{arr: {x: 1}}");
    checkDiff(preImage, fromjson("{arr: null}"), diff);

    // Case where 'arr' field is a scalar. The type mismatch implies that a future operation will
    // re-write the field, so it is set to null.
    preImage = fromjson("{arr: 'scalar!'}");
    checkDiff(preImage, fromjson("{arr: null}"), diff);

    // Case where the pre image 'arr' field is an object instead of an array. In this
    // situation, we know that some later operation will overwrite the field, so we set it to null
    // ensuring that it keeps its position in the document.
    preImage = fromjson("{dummyA: 1, arr: {foo: 1}, dummyB: 1}");
    checkDiff(preImage, fromjson("{dummyA: 1, arr: null, dummyB: 1}"), diff);

    // Case where pre image 'arr' field is a scalar. Again, we set it to null.
    preImage = fromjson("{dummyA: 1, arr: 1, dummyB: 1}");
    checkDiff(preImage, fromjson("{dummyA: 1, arr: null, dummyB: 1}"), diff);
}

TEST(DiffApplierTest, DiffWithDuplicateFields) {
    BSONObj diff = fromjson("{d: {dupField: false}, u: {dupField: 'new value'}}");
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), diff), DBException, 4728000);
}

TEST(DiffApplierTest, EmptyDiff) {
    BSONObj emptyDiff;
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), emptyDiff), DBException, 4770500);
}

TEST(DiffApplierTest, ArrayDiffAtTop) {
    BSONObj arrDiff = fromjson("{a: true, l: 5, 'd0': false}");
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), arrDiff), DBException, 4770503);
}

TEST(DiffApplierTest, DuplicateFieldNames) {
    // Within the same update type.
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{d: {a: false, b: false, a: false}}")),
        DBException,
        4728000);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{u: {f1: 3, f1: 4}}")), DBException, 4728000);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{i: {a: {}, a: null}}")), DBException, 4728000);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(),
                            fromjson("{sa: {d: {p: false}}, sa: {a: true, d: {p: false}}}")),
        DBException,
        4728000);

    // Across update types.
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{d: {b: false}, i: {a: {}, b: null}}")),
        DBException,
        4728000);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{u: {b: false}, i: {a: {}, b: null}}")),
        DBException,
        4728000);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{u: {a: {}}, sa: {d : {k: false}}}")),
        DBException,
        4728000);
}

}  // namespace
}  // namespace mongo::doc_diff
