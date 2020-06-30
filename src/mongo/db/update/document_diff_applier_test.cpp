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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo::doc_diff {
namespace {
/**
 * Checks that applying the diff (once or twice) to 'preImage' produces the expected post image.
 */
void checkDiff(const BSONObj& preImage, const BSONObj& expectedPost, const Diff& diff) {
    BSONObj postImage = applyDiff(preImage, diff);

    // This *MUST* check for binary equality, which is what we enforce between replica set
    // members. Logical equality (through woCompare() or ASSERT_BSONOBJ_EQ) is not enough to show
    // that the applier actually works.
    ASSERT_BSONOBJ_BINARY_EQ(postImage, expectedPost);

    BSONObj postImageAgain = applyDiff(postImage, diff);
    ASSERT_BSONOBJ_BINARY_EQ(postImageAgain, expectedPost);
}

TEST(DiffApplierTest, DeleteSimple) {
    const BSONObj preImage(BSON("f1" << 1 << "foo" << 2 << "f2" << 3));

    DocumentDiffBuilder builder;
    builder.addDelete("f1");
    builder.addDelete("f2");
    builder.addDelete("f3");

    auto diff = builder.serialize();

    checkDiff(preImage, BSON("foo" << 2), diff);
}

TEST(DiffApplierTest, InsertSimple) {
    const BSONObj preImage(BSON("f1" << 1 << "foo" << 2 << "f2" << 3));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData newField = "newField";

    DocumentDiffBuilder builder;
    builder.addInsert("f1", storage["a"]);
    builder.addInsert(newField, storage["b"]);

    auto diff = builder.serialize();
    checkDiff(preImage, BSON("foo" << 2 << "f2" << 3 << "f1" << 1 << "newField" << 2), diff);
}

TEST(DiffApplierTest, UpdateSimple) {
    const BSONObj preImage(BSON("f1" << 0 << "foo" << 2 << "f2" << 3));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData newField = "newField";

    DocumentDiffBuilder builder;
    builder.addUpdate("f1", storage["a"]);
    builder.addUpdate(newField, storage["b"]);

    auto diff = builder.serialize();
    checkDiff(preImage, BSON("f1" << 1 << "foo" << 2 << "f2" << 3 << "newField" << 2), diff);
}

TEST(DiffApplierTest, SubObjDiffSimple) {
    const BSONObj preImage(
        BSON("obj" << BSON("dField" << 0 << "iField" << 0 << "uField" << 0) << "otherField" << 0));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubObjDiff("obj");
        subBuilderGuard.builder()->addDelete("dField");
        subBuilderGuard.builder()->addInsert("iField", storage["a"]);
        subBuilderGuard.builder()->addUpdate("uField", storage["b"]);
    }

    auto diff = builder.serialize();
    checkDiff(
        preImage, BSON("obj" << BSON("uField" << 2 << "iField" << 1) << "otherField" << 0), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithAppend) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(999 << 999 << 999 << 999)));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData arr = "arr";

    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubArrDiff(arr);
        subBuilderGuard.builder()->addUpdate(1, storage["a"]);
        subBuilderGuard.builder()->addUpdate(4, storage["b"]);
    }

    auto diff = builder.serialize();

    checkDiff(preImage, BSON("arr" << BSON_ARRAY(999 << 1 << 999 << 999 << 2)), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithTruncate) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(999 << 999 << 999 << 999)));

    const BSONObj storage(BSON("a" << 1 << "b" << 2));
    StringData arr = "arr";

    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubArrDiff(arr);
        subBuilderGuard.builder()->addUpdate(1, storage["a"]);
        subBuilderGuard.builder()->setResize(3);
    }

    auto diff = builder.serialize();
    checkDiff(preImage, BSON("arr" << BSON_ARRAY(999 << 1 << 999)), diff);
}

TEST(DiffApplierTest, SubArrayDiffSimpleWithNullPadding) {
    const BSONObj preImage(BSON("arr" << BSON_ARRAY(0)));

    BSONObj storage(BSON("a" << 1));
    StringData arr = "arr";

    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubArrDiff(arr);
        subBuilderGuard.builder()->addUpdate(3, storage["a"]);
    }

    auto diff = builder.serialize();

    checkDiff(preImage, BSON("arr" << BSON_ARRAY(0 << NullLabeler{} << NullLabeler{} << 1)), diff);
}

TEST(DiffApplierTest, NestedSubObjUpdateScalar) {
    BSONObj storage(BSON("a" << 1));
    StringData subObj = "subObj";
    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubObjDiff(subObj);
        {
            auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(subObj);
            subSubBuilderGuard.builder()->addUpdate("a", storage["a"]);
        }
    }

    auto diff = builder.serialize();

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

    DocumentDiffBuilder builder;
    {
        builder.addDelete(dFieldA);
        builder.addDelete(dFieldB);
        builder.addUpdate(uField, storage["uFieldNew"]);

        auto subBuilderGuard = builder.startSubArrDiff(arr);
        {
            {
                auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(1);
                subSubBuilderGuard.builder()->addUpdate("a", storage["a"]);
            }

            {
                auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(2);
                subSubBuilderGuard.builder()->addUpdate("b", storage["b"]);
            }

            {
                auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(3);
                subSubBuilderGuard.builder()->addUpdate("c", storage["c"]);
            }
            subBuilderGuard.builder()->addUpdate(4, storage["newObj"]);
        }
    }

    auto diff = builder.serialize();

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
    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubArrDiff(arr);
        {
            subBuilderGuard.builder()->addUpdate(1, storage["dummyA"]);
            subBuilderGuard.builder()->addUpdate(2, storage["dummyB"]);
            subBuilderGuard.builder()->addUpdate(5, storage["dummyC"]);
        }
    }

    auto diff = builder.serialize();

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
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), diff), DBException, 4728000);
}

TEST(DiffApplierTest, EmptyDiff) {
    BSONObj emptyDiff;
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), emptyDiff), DBException, 4770500);
}

TEST(DiffApplierTest, ArrayDiffAtTop) {
    BSONObj arrDiff = fromjson("{a: true, l: 5, 'd0': false}");
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), arrDiff), DBException, 4770503);
}

TEST(DiffApplierTest, DuplicateFieldNames) {
    // Within the same update type.
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{d: {a: false, b: false, a: false}}")),
                       DBException,
                       4728000);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{u: {f1: 3, f1: 4}}")), DBException, 4728000);
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{i: {a: {}, a: null}}")), DBException, 4728000);
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{s: {a: {d: {p: false}}, a: {a: true, d: {p: false}}}}")),
        DBException,
        4728000);

    // Across update types.
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{d: {b: false}, i: {a: {}, b: null}}")),
                       DBException,
                       4728000);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{u: {b: false}, i: {a: {}, b: null}}")),
                       DBException,
                       4728000);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{u: {a: {}}, s: {a: {d : {k: false}}}}")),
                       DBException,
                       4728000);
}

}  // namespace
}  // namespace mongo::doc_diff
