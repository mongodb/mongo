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
    builder.addDelete("f1"_sd);
    builder.addDelete("f2"_sd);
    builder.addDelete("f3"_sd);

    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{d: {f1: false, f2: false, f3: false}}"), out);

    DocumentDiffReader reader(out);
    ASSERT_EQ(*reader.nextDelete(), "f1");
    ASSERT_EQ(*reader.nextDelete(), "f2");
    ASSERT_EQ(*reader.nextDelete(), "f3");
    ASSERT(reader.nextDelete() == boost::none);

    ASSERT(reader.nextInsert() == boost::none);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, InsertSimple) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    DocumentDiffBuilder builder;
    builder.addInsert("f1"_sd, kDummyObj["a"]);
    builder.addInsert("f2"_sd, kDummyObj["b"]);

    auto out = builder.release();
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
    builder.addUpdate("f1"_sd, kDummyObj["a"]);
    builder.addUpdate("f2"_sd, kDummyObj["b"]);

    auto out = builder.release();
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
        DocumentDiffBuilder sub(builder.startSubObjDiff("obj"));
        sub.addInsert("iField", kDummyObj["a"]);
        sub.addUpdate("uField", kDummyObj["b"]);
        sub.addDelete("dField");
    }

    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson("{s :"
                 "{obj: {d : { dField: false }, u : { uField: 'foo' }, i : { iField: 1}}}}"));

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
        ArrayDiffBuilder sub(builder.startSubArrDiff("arr"));
        sub.addUpdate(0, kDummyObj["b"]);
        {
            DocumentDiffBuilder subSub(sub.startSubObjDiff(2));
            subSub.addDelete("dField");
        }

        sub.addUpdate(5, kDummyObj["a"]);
        sub.setResize(6);
    }

    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(out,
                             fromjson("{s: {arr: {a: true, l: 6,"
                                      "'0': {'u': {foo: 'bar'}}, "
                                      "'2': {s: {'d': {dField: false}}},"
                                      "'5': {u: 1}}}}"));

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
        ArrayDiffBuilder subArr(builder.startSubArrDiff("arr"));
        {
            {
                DocumentDiffBuilder subObjBuilder(subArr.startSubObjDiff(1));
                subObjBuilder.addUpdate("a", storage["a"]);
            }

            {
                DocumentDiffBuilder subObjBuilder(subArr.startSubObjDiff(2));
                subObjBuilder.addUpdate("b", storage["b"]);
            }

            {
                DocumentDiffBuilder subObjBuilder(subArr.startSubObjDiff(3));
                subObjBuilder.addUpdate("c", storage["c"]);
            }
        }
    }

    const auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(out,
                             fromjson("{s: {arr: {a: true, '1': {s: {u: {a: 1}}},"
                                      "'2': {s: {u: {b: 2}}}, '3': {s: {u: {c: 3}}}}}}"));
}

TEST(DiffSerializationTest, SubArrayHighIndex) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    DocumentDiffBuilder builder;

    {
        ArrayDiffBuilder sub(builder.startSubArrDiff("arr"));
        sub.addUpdate(254, kDummyObj["b"]);
    }

    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{s: {arr: {a: true, '254': {u: 'foo'}}}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "arr");

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
        DocumentDiffBuilder sub(builder.startSubObjDiff("obj"));
        sub.abandon();
    }

    // Make sure that after we abandon something we can still use the parent builder.
    builder.addDelete("dField2");

    {
        DocumentDiffBuilder sub(builder.startSubObjDiff("obj2"));
        sub.addDelete("dField2");
    }

    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson("{d : {dField1: false, dField2: false}, u: {uField: 1}, s: {obj2: {d: "
                 "{dField2: false}}}}"));
}

TEST(DiffSerializationTest, SubArrayDiffAbandon) {
    const BSONObj kDummyObj(BSON("a" << 1));

    DocumentDiffBuilder builder;
    builder.addDelete("dField1");
    builder.addUpdate("uField", kDummyObj["a"]);

    {
        ArrayDiffBuilder sub(builder.startSubArrDiff("arr"));
        sub.abandon();
    }

    // Make sure that after we abandon something we can still use the parent builder.
    builder.addDelete("dField2");

    {
        ArrayDiffBuilder sub(builder.startSubArrDiff("arr2"));
        sub.setResize(5);
    }
    auto out = builder.release();
    ASSERT_BSONOBJ_BINARY_EQ(out,
                             fromjson("{d : {dField1: false, dField2: false}, u: {uField: 1},"
                                      "s: {arr2: {a: true, l: 5}}}"));
}

TEST(DiffSerializationTest, ValidateComputeApproxSize) {
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

    DocumentDiffBuilder builder;
    builder.addDelete("deleteField");
    builder.addInsert("insert", storage["num"]);
    builder.addUpdate("update1", storage["subObj"]);
    builder.addDelete("");
    {
        // Ensure size of the sub-array diff is included.
        auto subDiff = builder.startSubArrDiff("subArray");
        subDiff.setResize(5);
        subDiff.addUpdate(2, storage["num"]);
        subDiff.addUpdate(2, storage["str"]);

        auto subSubDiff = subDiff.startSubObjDiff(22);
        subSubDiff.addInsert("insert2", storage["emptyStr"]);
        subSubDiff.addUpdate("update3", storage["null"]);
    }
    {
        // Ensure size of the sub-object diff is included.
        auto subDiff = builder.startSubObjDiff("subObj");
        subDiff.addUpdate("setArray", storage["array"]);
    }
    // Update with a sub-object.
    builder.addUpdate("update4", storage["subObj"]);

    auto computedSize = builder.computeApproxSize();
    auto out = builder.release();
    ASSERT_EQ(computedSize, out.objsize());
}

}  // namespace
}  // namespace mongo::doc_diff
