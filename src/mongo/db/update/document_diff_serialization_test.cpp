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


#include "mongo/db/update/document_diff_serialization.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_test_helpers.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <exception>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


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
    diff_tree::DocumentSubDiffNode diffNode;
    StringData fieldName1 = "f1";
    StringData fieldName2 = "f2";
    StringData fieldName3 = "f3";
    diffNode.addDelete(fieldName1);
    diffNode.addDelete(fieldName2);
    diffNode.addDelete(fieldName3);

    auto out = diffNode.serialize();
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

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addInsert("f1", kDummyObj["a"]);
    diffNode.addInsert("f2", kDummyObj["b"]);

    auto out = diffNode.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{'i': {f1: 1, f2: 'foo' }}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextInsert()->binaryEqual(withFieldName(kDummyObj["a"], "f1")));
    ASSERT(reader.nextInsert()->binaryEqual(withFieldName(kDummyObj["b"], "f2")));
    ASSERT(reader.nextInsert() == boost::none);

    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, BinarySimple) {
    const BSONObj kDummyObj(BSON(
        "a" << BSON("o" << 0 << "d" << BSONBinData("abcdef", 6, BinDataType::BinDataGeneral))));

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addBinary("b1", kDummyObj["a"]);

    auto out = diffNode.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        BSON("b" << BSON("b1" << BSON("o"
                                      << 0 << "d"
                                      << BSONBinData("abcdef", 6, BinDataType::BinDataGeneral)))));


    DocumentDiffReader reader(out);
    ASSERT(reader.nextBinary()->binaryEqual(withFieldName(kDummyObj["a"], "b1")));
    ASSERT(reader.nextBinary() == boost::none);

    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextSubDiff() == boost::none);
}

TEST(DiffSerializationTest, UpdateSimple) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));

    diff_tree::DocumentSubDiffNode diffNode;
    diffNode.addUpdate("f1", kDummyObj["a"]);
    diffNode.addUpdate("f2", kDummyObj["b"]);

    auto out = diffNode.serialize();
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

    diff_tree::DocumentSubDiffNode diffNode;
    auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();

    subDiffNode->addInsert("iField", kDummyObj["a"]);
    subDiffNode->addUpdate("uField", kDummyObj["b"]);
    subDiffNode->addDelete("dField");
    diffNode.addChild("obj", std::move(subDiffNode));
    auto out = diffNode.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out, fromjson("{sobj: {d : { dField: false }, u : { uField: 'foo' }, i : { iField: 1}}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "obj");
        auto subReader = get<DocumentDiffReader>(subDiffVar);

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
    const BSONObj kDummyObj(BSON("a" << 1 << "b" << BSON("foo" << "bar")));
    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(0, kDummyObj["b"]);
        {
            auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
            subSubDiffNode->addDelete("dField");
            subDiffNode->addChild(2, std::move(subSubDiffNode));
        }

        subDiffNode->addUpdate(5, kDummyObj["a"]);
        subDiffNode->setResize(6);
        diffNode.addChild("arr", std::move(subDiffNode));
    }

    auto out = diffNode.serialize();
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
        auto subReader = get<ArrayDiffReader>(subDiffVar);

        // There should be an 'update' at index 0.
        {
            auto [index, mod] = *subReader.next();
            ASSERT_EQ(index, 0);
            ASSERT(get<BSONElement>(mod).binaryEqualValues(kDummyObj["b"]));
        }

        // There should be a sub-diff at index 2.
        {
            auto [index, subSubReaderVar] = *subReader.next();
            ASSERT_EQ(index, 2);
            auto subSubReader = get<DocumentDiffReader>(subSubReaderVar);

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
            ASSERT(get<BSONElement>(mod).binaryEqualValues(kDummyObj["a"]));

            ASSERT(subReader.next() == boost::none);
        }

        ASSERT(*subReader.newSize() == 6);
    }
}

TEST(DiffSerializationTest, SubArrayNestedObject) {
    BSONObj storage(BSON("a" << 1 << "b" << 2 << "c" << 3));
    diff_tree::DocumentSubDiffNode diffNode;
    {
        auto subArrDiffNode = std::make_unique<diff_tree::ArrayNode>();
        {
            {
                auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
                subDiffNode->addUpdate(storage["a"].fieldNameStringData(), storage["a"]);
                subArrDiffNode->addChild(1, std::move(subDiffNode));
            }

            {
                auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
                subDiffNode->addUpdate(storage["b"].fieldNameStringData(), storage["b"]);
                subArrDiffNode->addChild(2, std::move(subDiffNode));
            }

            {
                auto subDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
                subDiffNode->addUpdate(storage["c"].fieldNameStringData(), storage["c"]);
                subArrDiffNode->addChild(3, std::move(subDiffNode));
            }
        }
        diffNode.addChild("subArray", std::move(subArrDiffNode));
    }

    const auto out = diffNode.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(
        out,
        fromjson(
            "{ssubArray: {a: true, 's1': {u: {a: 1}}, 's2': {u: {b: 2}}, 's3': {u: {c: 3}}}}"));
}

TEST(DiffSerializationTest, SubArrayHighIndex) {
    const BSONObj kDummyObj(BSON("a" << 1 << "b"
                                     << "foo"));
    diff_tree::DocumentSubDiffNode diffNode;

    {
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->addUpdate(254, kDummyObj["b"]);
        diffNode.addChild("subArray", std::move(subDiffNode));
    }

    auto out = diffNode.serialize();
    ASSERT_BSONOBJ_BINARY_EQ(out, fromjson("{ssubArray: {a: true, 'u254': 'foo'}}"));

    DocumentDiffReader reader(out);
    ASSERT(reader.nextUpdate() == boost::none);
    ASSERT(reader.nextDelete() == boost::none);
    ASSERT(reader.nextInsert() == boost::none);

    {
        auto [name, subDiffVar] = *reader.nextSubDiff();
        ASSERT_EQ(name, "subArray");

        auto subReader = get<ArrayDiffReader>(subDiffVar);

        // There should be an 'update' at index 254.
        {
            auto [index, bsonElem] = *subReader.next();
            ASSERT_EQ(index, 254);
            ASSERT(get<BSONElement>(bsonElem).binaryEqualValues(kDummyObj["b"]));
        }

        ASSERT(subReader.newSize() == boost::none);
    }
}

TEST(DiffSerializationTest, ValidateComputeApproxSize) {
    const size_t padding = 25;
    const auto storage =
        BSON("num" << 4 << "str"
                   << "val"
                   << "emptyStr"
                   << ""
                   << "null" << BSONNULL << "array" << BSON_ARRAY("val1" << "val2" << 3) << "subObj"
                   << BSON("" << "update"));

    diff_tree::DocumentSubDiffNode diffNode(padding);
    diffNode.addDelete("deleteField");
    diffNode.addInsert("insert", storage["num"]);
    diffNode.addUpdate("update1", storage["subObj"]);
    diffNode.addDelete("");

    // Ensure size of the sub-array diff is included.
    auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
    subDiffNode->setResize(5);
    subDiffNode->addUpdate(2, storage["num"]);
    subDiffNode->addUpdate(3, storage["str"]);
    {
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subSubDiffNode->addInsert("insert2", storage["emptyStr"]);
        subSubDiffNode->addUpdate("update3", storage["null"]);
        subDiffNode->addChild(5, std::move(subSubDiffNode));
    }
    {
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subSubDiffNode->addInsert("subObj", storage["subObj"]);
        subSubDiffNode->addUpdate("update3", storage["null"]);
        subDiffNode->addChild(234, std::move(subSubDiffNode));
    }
    {
        auto subSubArrayBuilder = std::make_unique<diff_tree::ArrayNode>();
        subSubArrayBuilder->setResize(10000);
        subSubArrayBuilder->addUpdate(0, storage["array"]);
        subDiffNode->addChild(2456, std::move(subSubArrayBuilder));
    }
    {
        auto subArrayBuilder = std::make_unique<diff_tree::ArrayNode>();

        subArrayBuilder->addUpdate(2, storage["str"]);
        diffNode.addChild("subArray2", std::move(subArrayBuilder));
    }
    {
        // Ensure size of the sub-object diff is included.
        auto subsubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        subsubDiffNode->addUpdate("setArray", storage["array"]);
        diffNode.addChild("subObj", std::move(subsubDiffNode));
    }
    diffNode.addChild("subArray", std::move(subDiffNode));
    // Update with a sub-object.
    diffNode.addUpdate("update2", storage["subObj"]);

    auto computedSize = diffNode.getObjSize();
    auto out = diffNode.serialize();
    ASSERT_EQ(computedSize, out.objsize() + padding);
}

TEST(DiffSerializationTest, ExecptionsWhileDiffBuildingDoesNotLeakMemory) {
    try {
        diff_tree::DocumentSubDiffNode diffNode;
        auto subDiffNode = std::make_unique<diff_tree::ArrayNode>();
        subDiffNode->setResize(4);
        auto subSubDiffNode = std::make_unique<diff_tree::DocumentSubDiffNode>();
        diffNode.addChild("asdf", std::move(subSubDiffNode));
        throw std::exception();
    } catch (const std::exception&) {
    }
}
TEST(DiffSerializationTest, UnexpectedFieldsInObjDiff) {
    // Empty field names on top level.
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{d: {f: false}, '' : {}}")), DBException, 4770505);
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), fromjson("{'' : {}}")), DBException, 4770505);

    // Expected diff to be non-empty.
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{d: {f: false}, s : {}}")), DBException, 4770500);
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), fromjson("{sa : {}}")), DBException, 4770500);

    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), fromjson("{p : 1}")), DBException, 4770503);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{u : true}")), DBException, 4770507);
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), fromjson("{d : []}")), DBException, 4770507);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{i : null}")), DBException, 4770507);

    // If the order of the fields is not obeyed.
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{i : {}, d: {}}")), DBException, 4770503);

    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{s : {i: {}}, i: {}}")), DBException, 4770514);
    ASSERT_THROWS_CODE(applyDiffTestHelper(BSONObj(), fromjson("{sa : {u: {}}, d: {p: false}}")),
                       DBException,
                       4770514);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{sa : {u: {}}, sb : {u: {}}, i: {}}")),
        DBException,
        4770514);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(BSONObj(), fromjson("{sa : {u: {}}, p: {}}")), DBException, 4770514);

    // Empty deletes object is valid.
    ASSERT_BSONOBJ_BINARY_EQ(applyDiffTestHelper(fromjson("{a: 1}"), fromjson("{d : {}}")),
                             fromjson("{a: 1}"));
}

TEST(DiffSerializationTest, UnexpectedFieldsInArrayDiff) {
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: true, '3u' : 1}}")),
        DBException,
        4770512);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: true, u : true}}")),
        DBException,
        4770521);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: true, '5' : {}}}")),
        DBException,
        4770521);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: false, 'u3' : 4}}")),
        DBException,
        4770520);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: 1, 'u3' : 4}}")),
        DBException,
        4770519);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: true, 's3' : 4}}")),
        DBException,
        4770501);
    ASSERT_THROWS_CODE(
        applyDiffTestHelper(fromjson("{arr: []}"), fromjson("{sarr: {a: true, 'd3' : 4}}")),
        DBException,
        4770502);
}

}  // namespace
}  // namespace mongo::doc_diff
