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
#include "mongo/unittest/unittest.h"

namespace mongo::doc_diff {
namespace {

// Convenient (but inefficient) way to rename a BSONElement. The returned BSONElement is not safe
// to use after another call to withFieldName(). Note that this is not thread-safe so do not
// copy/paste this into real server code.
BSONElement withFieldName(BSONElement elem, StringData fieldName) {
    static BSONObj storage;
    storage = elem.wrap(fieldName);

    return storage.firstElement();
}

TEST(DiffSerializationTest, DeleteSimple) {
    DocumentDiffBuilder builder;
    StringData fieldName1 = "f1";
    StringData fieldName2 = "f2";
    StringData fieldName3 = "f3";
    builder.addDelete(fieldName1);
    builder.addDelete(fieldName2);
    builder.addDelete(fieldName3);

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{d: {f1: false, f2: false, f3: false}}"), out);

    DocumentDiffReader reader(out);
    ASSERT_EQ(*reader.nextDelete(), fieldName1);
    ASSERT_EQ(*reader.nextDelete(), fieldName2);
    ASSERT_EQ(*reader.nextDelete(), fieldName3);
    ASSERT(reader.nextDelete() == boost::none);

    ASSERT(reader.nextInsert() == boost::none);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, InsertSimple) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    DocumentDiffBuilder builder;
    builder.addInsert("f1", kDummyObj["a"]);
    builder.addInsert("f2", kDummyObj["b"]);

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{'i': {f1: 1, f2: 'foo' }}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextInsert()->binaryEqual(withFieldName(kDummyObj["a"], "f1")));
    ASSERT(reader.nextInsert()->binaryEqual(withFieldName(kDummyObj["b"], "f2")));
    ASSERT(reader.nextInsert() == boost::none);

    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, UpdateSimple) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    DocumentDiffBuilder builder;
    builder.addUpdate("f1", kDummyObj["a"]);
    builder.addUpdate("f2", kDummyObj["b"]);

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{'u': { f1: 1, f2: 'foo'}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate()->binaryEqual(withFieldName(kDummyObj["a"], "f1")));
    ASSERT(reader.nextUpdate()->binaryEqual(withFieldName(kDummyObj["b"], "f2")));
    ASSERT(reader.nextUpdate() == boost::none);

    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, SubDiff) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubObjDiff("obj");

        subBuilderGuard.builder()->addInsert("iField", kDummyObj["a"]);
        subBuilderGuard.builder()->addUpdate("uField", kDummyObj["b"]);
        subBuilderGuard.builder()->addDelete("dField");
    }

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out, fromjson("{sobj: {d : { dField: false }, u : { uField: 'foo' }, i : { iField: 1}}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "obj");
        auto subReader = stdx::get<DocumentDiffReader>(subDiffVar);

        ASSERT_EQ(*subReader.nextDelete(), "dField");
        ASSERT(subReader.nextDelete() == boost::none);

        ASSERT(subReader.nextUpdate()->binaryEqual(withFieldName(kDummyObj["b"], "uField")));
        ASSERT(subReader.nextUpdate() == boost::none);

        ASSERT(subReader.nextInsert()->binaryEqual(withFieldName(kDummyObj["a"], "iField")));
        ASSERT(subReader.nextInsert() == boost::none);
    }

    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, SubArrayWithSubDiff) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << BSON("foo"
                                             << "bar")));
    DocumentDiffBuilder builder;
    {
        auto subBuilderGuard = builder.startSubArrDiff("arr");
        subBuilderGuard.builder()->addUpdate(0, kDummyObj["b"]);
        {
            auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(2);
            subSubBuilderGuard.builder()->addDelete("dField");
        }

        subBuilderGuard.builder()->addUpdate(5, kDummyObj["a"]);
        subBuilderGuard.builder()->setResize(6);
    }

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out,
                             fromjson("{sarr: {a: true, l: 6,"
                                      "'u0': {foo: 'bar'}, "
                                      "'s2': {'d': {dField: false}},"
                                      "'u5': 1}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "arr");
        auto subReader = stdx::get<ArrayDiffReader>(subDiffVar);

        // There should be an 'update' at index 0.
        {
            auto [index, mod] = *subReader.next();
            ASSERT_EQ(index, 0);
            ASSERT(stdx::get<BSONElement>(mod).binaryEqualValues(kDummyObj["b"]));
        }

        // There should be a sub-diff at index 2.
        {
            auto [index, subSubReaderVar] = *subReader.next();
            ASSERT_EQ(index, 2);
            auto subSubReader = stdx::get<DocumentDiffReader>(subSubReaderVar);

            ASSERT_EQ(*subSubReader.nextDelete(), "dField");
            ASSERT(subSubReader.nextDelete() == boost::none);
            ASSERT(subSubReader.nextInsert() == boost::none);
            ASSERT(subSubReader.nextUpdate() == boost::none);
            ASSERT(subSubReader.nextSubDiff() == boost::none);
        }

        // There should be an update at index 5.
        {
            auto [index, mod] = *subReader.next();
            ASSERT_EQ(index, 5);
            ASSERT(stdx::get<BSONElement>(mod).binaryEqualValues(kDummyObj["a"]));

            ASSERT(subReader.next() == boost::none);
        }

        ASSERT(*subReader.newSize() == 6);
    }
}

TEST(DiffSerializationTest, SubArrayNestedObject) {
    BSONObj storage(BSON("a" << 1 << "b" << 2 << "c" << 3));
    DocumentDiffBuilder builder;
    {
        auto subArrBuilderGuard = builder.startSubArrDiff("subArray");
        {
            {
                auto subBuilderGuard = subArrBuilderGuard.builder()->startSubObjDiff(1);
                subBuilderGuard.builder()->addUpdate(storage["a"].fieldNameStringData(),
                                                     storage["a"]);
            }

            {
                auto subBuilderGuard = subArrBuilderGuard.builder()->startSubObjDiff(2);
                subBuilderGuard.builder()->addUpdate(storage["b"].fieldNameStringData(),
                                                     storage["b"]);
            }

            {
                auto subBuilderGuard = subArrBuilderGuard.builder()->startSubObjDiff(3);
                subBuilderGuard.builder()->addUpdate(storage["c"].fieldNameStringData(),
                                                     storage["c"]);
            }
        }
    }

    const auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson(
            "{ssubArray: {a: true, 's1': {u: {a: 1}}, 's2': {u: {b: 2}}, 's3': {u: {c: 3}}}}"));
}

TEST(DiffSerializationTest, SubArrayHighIndex) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));
    DocumentDiffBuilder builder;

    {
        auto subBuilderGuard = builder.startSubArrDiff("subArray");
        subBuilderGuard.builder()->addUpdate(254, kDummyObj["b"]);
    }

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{ssubArray: {a: true, 'u254': 'foo'}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "subArray");

        auto subReader = stdx::get<ArrayDiffReader>(subDiffVar);

        // There should be an 'update' at index 254.
        {
            auto [index, bsonElem] = *subReader.next();
            ASSERT_EQ(index, 254);
            ASSERT(stdx::get<BSONElement>(bsonElem).binaryEqualValues(kDummyObj["b"]));
        }

        ASSERT(subReader.newSize() == boost::none);
    }
}

TEST(DiffSerializationTest, SubDiffAbandon) {
    const BSONObj kDummyObj(BSON("a" << 1));

    DocumentDiffBuilder builder;
    builder.addDelete("dField1");
    builder.addUpdate("uField", kDummyObj["a"]);

    {

        auto subBuilderGuard = builder.startSubObjDiff("obj");
        subBuilderGuard.builder()->addDelete("dField3");
        subBuilderGuard.abandon();
    }

    // Make sure that after we abandon something we can still use the parent builder.
    builder.addDelete("dField2");

    {
        auto subBuilderGuard = builder.startSubObjDiff("obj2");
        subBuilderGuard.builder()->addDelete("dField2");
    }

    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson("{d: {dField1: false, dField2: false}, u: {uField: 1}, sobj2: {d: "
                 "{dField2: false}}}"));
}

TEST(DiffSerializationTest, SubArrayDiffAbandon) {
    const BSONObj kDummyObj(BSON("a" << 1));

    DocumentDiffBuilder builder;
    builder.addDelete("dField1");
    builder.addUpdate("uField", kDummyObj["a"]);

    {
        auto subBuilderGuard = builder.startSubArrDiff("subArray");
        subBuilderGuard.builder()->addUpdate(4, kDummyObj.firstElement());
        subBuilderGuard.abandon();
    }

    // Make sure that after we abandon something we can still use the parent builder.
    builder.addDelete("dField2");

    {
        auto subBuilderGuard = builder.startSubArrDiff("arr2");
        subBuilderGuard.builder()->setResize(5);
    }
    auto out = builder.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson("{d : {dField1: false, dField2: false}, u: {uField: 1}, sarr2: {a: true, l: 5}}"));
}

TEST(DiffSerializationTest, ValidateComputeApproxSize) {
    const size_t padding = 20;
    const auto storage = BSON("num" << 4 << "str"
                                    << "val"
                                    << "emptyStr"
                                    << ""
                                    << "null" << BSONNULL << "array"
                                    << BSON_ARRAY("val1"
                                                  << "val2" << 3)
                                    << "subObj"
                                    << BSON(""
                                            << "update"));

    DocumentDiffBuilder builder(padding);
    builder.addDelete("deleteField");
    builder.addInsert("insert", storage["num"]);
    builder.addUpdate("update1", storage["subObj"]);
    builder.addDelete("");
    {
        // Ensure size of the sub-array diff is included.
        auto subBuilderGuard = builder.startSubArrDiff("subArray");
        subBuilderGuard.builder()->setResize(5);
        subBuilderGuard.builder()->addUpdate(2, storage["num"]);
        subBuilderGuard.builder()->addUpdate(2, storage["str"]);
        {
            auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(2);
            subSubBuilderGuard.builder()->addInsert("insert2", storage["emptyStr"]);
            subSubBuilderGuard.builder()->addUpdate("update3", storage["null"]);
        }
        {
            auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(234);
            subSubBuilderGuard.builder()->addInsert("subObj", storage["subObj"]);
            subSubBuilderGuard.builder()->addUpdate("update3", storage["null"]);
        }
        {
            auto subSubArrayBuilderGuard = subBuilderGuard.builder()->startSubArrDiff(2456);
            subSubArrayBuilderGuard.builder()->addUpdate(0, storage["num"]);
            subSubArrayBuilderGuard.abandon();
        }
        {
            auto subSubArrayBuilderGuard = subBuilderGuard.builder()->startSubArrDiff(2456);
            subSubArrayBuilderGuard.builder()->setResize(10000);
            subSubArrayBuilderGuard.builder()->addUpdate(0, storage["array"]);
        }
    }
    {
        auto subArrayBuilderGuard = builder.startSubArrDiff("subArray2");
        subArrayBuilderGuard.builder()->addUpdate(2, storage["str"]);
    }
    {
        // Ensure size of the sub-object diff is included.
        auto subBuilderGuard = builder.startSubObjDiff("subObj");
        subBuilderGuard.builder()->addUpdate("setArray", storage["array"]);
    }
    {
        // Ensure size of the abandoned sub-object diff is non included.
        auto subBuilderGuard = builder.startSubObjDiff("subObj1");
        subBuilderGuard.builder()->addUpdate("setArray2", storage["array"]);
        subBuilderGuard.abandon();
    }
    // Update with a sub-object.
    builder.addUpdate("update2", storage["subObj"]);

    auto computedSize = builder.getObjSize();
    auto out = builder.serialize();
    ASSERT_EQ(computedSize, out.objsize() + padding);
}

TEST(DiffSerializationTest, ExecptionsWhileDiffBuildingDoesNotLeakMemory) {
    try {
        DocumentDiffBuilder builder;
        auto subBuilderGuard = builder.startSubArrDiff("subArray");
        subBuilderGuard.builder()->setResize(4);
        auto subSubBuilderGuard = subBuilderGuard.builder()->startSubObjDiff(5);
        throw std::exception();
    } catch (const std::exception&) {
    }
}
TEST(DiffSerializationTest, UnexpectedFieldsInObjDiff) {
    // Empty field names on top level.
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{d: {f: false}, '' : {}}")), DBException, 4770505);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{'' : {}}")), DBException, 4770505);

    // Expected diff to be non-empty.
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{d: {f: false}, s : {}}")), DBException, 4770500);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{sa : {}}")), DBException, 4770500);

    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{p : 1}")), DBException, 4770503);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{u : true}")), DBException, 4770507);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{d : []}")), DBException, 4770507);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{i : null}")), DBException, 4770507);

    // If the order of the fields is not obeyed.
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{i : {}, d: {}}")), DBException, 4770503);

    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{s : {i: {}}, i: {}}")), DBException, 4770514);
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{sa : {u: {}}, d: {p: false}}")), DBException, 4770514);
    ASSERT_THROWS_CODE(applyDiff(BSONObj(), fromjson("{sa : {u: {}}, sb : {u: {}}, i: {}}")),
                       DBException,
                       4770514);
    ASSERT_THROWS_CODE(
        applyDiff(BSONObj(), fromjson("{sa : {u: {}}, p: {}}")), DBException, 4770514);

    // Empty deletes object is valid.
    ASSERT_BSONOBJ_BINARY_EQ(applyDiff(fromjson("{a: 1}"), fromjson("{d : {}}")),
                             fromjson("{a: 1}"));
}

TEST(DiffSerializationTest, UnexpectedFieldsInArrayDiff) {
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: true, '3u' : 1}}")),
                       DBException,
                       4770512);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: true, u : true}}")),
                       DBException,
                       4770521);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: true, '5' : {}}}")),
                       DBException,
                       4770521);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: false, 'u3' : 4}}")),
                       DBException,
                       4770520);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: 1, 'u3' : 4}}")),
                       DBException,
                       4770519);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: true, 's3' : 4}}")),
                       DBException,
                       4770501);
    ASSERT_THROWS_CODE(applyDiff(fromjson("{arr: []}"), fromjson("{sarr: {a: true, 'd3' : 4}}")),
                       DBException,
                       4770502);
}

}  // namespace
}  // namespace mongo::doc_diff
