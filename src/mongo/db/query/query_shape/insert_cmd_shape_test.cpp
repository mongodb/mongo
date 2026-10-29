// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/insert_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::query_shape {
namespace {
using namespace std::literals::string_view_literals;

using write_ops::InsertCommandRequest;

const auto kTestNss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");

class InsertCmdShapeTest : public ServiceContextTest {
public:
    InsertCmdShape makeShape(std::string_view insertCmd) {
        auto icr = InsertCommandRequest::parseOwned(fromjson(insertCmd));
        return InsertCmdShape(icr);
    }

    QueryShapeHash makeShapeHash(std::string_view insertCmd) {
        auto shape = makeShape(insertCmd);
        return shape.sha256Hash(expCtx->getOperationContext(), {});
    }

    const boost::intrusive_ptr<ExpressionContext> expCtx =
        make_intrusive<ExpressionContextForTest>();
};

/**
 * QUERY SHAPE FIELDS
 */

// Test that a basic insert command produces the expected shape: namespace and documents as a fixed
// placeholder. The documents field is always a single-element array containing a shapified object.
// In representative mode, documents appears as [ { "?": "?" } ].
// In debug mode, documents appears as "?array<?object>" reflecting the array-of-objects type.
TEST_F(InsertCmdShapeTest, DefaultInsertShape) {
    auto shape = makeShape(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");

    const auto expectedRepresentativeShape = fromjson(R"({
        cmdNs: { db: "testdb", coll: "testcoll" },
        command: "insert",
        documents: [ { "?": "?" } ]
    })");
    ASSERT_BSONOBJ_EQ(
        expectedRepresentativeShape,
        shape.toBson(expCtx->getOperationContext(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     {}));

    const auto expectedDebugShape = fromjson(R"({
        cmdNs: { db: "testdb", coll: "testcoll" },
        command: "insert",
        documents: "?array<?object>"
    })");
    ASSERT_BSONOBJ_EQ(
        expectedDebugShape,
        shape.toBson(expCtx->getOperationContext(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     {}));
}

// Test that documents with different field names produce the same shape.
TEST_F(InsertCmdShapeTest, DocumentsIsAlwaysPlaceholderRegardlessOfFieldNames) {
    auto shape1 = makeShape(R"({
        insert: "testcoll",
        documents: [ { a: 1 } ],
        "$db": "testdb"
    })");
    auto shape2 = makeShape(R"({
        insert: "testcoll",
        documents: [ { b: "hello", c: true } ],
        "$db": "testdb"
    })");
    ASSERT_BSONOBJ_EQ(
        shape1.toBson(expCtx->getOperationContext(),
                      query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      {}),
        shape2.toBson(expCtx->getOperationContext(),
                      query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      {}));
}

// Test that multiple documents produce the same shape as a single document.
TEST_F(InsertCmdShapeTest, DocumentsIsAlwaysPlaceholderRegardlessOfDocumentCount) {
    auto shape1 = makeShape(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    auto shape2 = makeShape(R"({
        insert: "testcoll",
        documents: [ { x: 1 }, { y: 2 }, { z: 3 } ],
        "$db": "testdb"
    })");
    ASSERT_BSONOBJ_EQ(
        shape1.toBson(expCtx->getOperationContext(),
                      query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      {}),
        shape2.toBson(expCtx->getOperationContext(),
                      query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      {}));
}

// Test that the debug format represents documents with a type placeholder.
TEST_F(InsertCmdShapeTest, InsertShapeDebugFormat) {
    auto shape = makeShape(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    const auto bson =
        shape.toBson(expCtx->getOperationContext(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     {});
    // Verify 'documents' is present and shapified (not the original value).
    ASSERT_TRUE(bson.hasField("documents"));
    ASSERT_FALSE(bson["documents"].isABSONObj() && bson["documents"].Obj().hasField("x"));
}

/**
 * QUERY SHAPE HASH
 */

// Test that inserts with different document contents hash to the same value (documents is always
// placeholder).
TEST_F(InsertCmdShapeTest, DifferentDocumentsSameHash) {
    const auto hash1 = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { a: 1 } ],
        "$db": "testdb"
    })");
    const auto hash2 = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { b: "hello", c: true }, { d: 42 } ],
        "$db": "testdb"
    })");
    ASSERT_EQ(hash1, hash2);
}

// Test that inserts targeting different collections hash to different values.
TEST_F(InsertCmdShapeTest, DifferentNamespacesDifferentHash) {
    const auto hash1 = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    const auto hash2 = makeShapeHash(R"({
        insert: "othercoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    ASSERT_NOT_EQUALS(hash1, hash2);
}

// Test that the same insert command hashes to the same value when called twice.
TEST_F(InsertCmdShapeTest, SameInsertSameHash) {
    const auto hash1 = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    const auto hash2 = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    ASSERT_EQ(hash1, hash2);
}

/**
 * QUERY SHAPE SIZE
 */

// Test that InsertCmdShapeComponents has a fixed size (no variable-length fields).
TEST_F(InsertCmdShapeTest, ShapeComponentsSizeIsFixed) {
    InsertCmdShapeComponents components;
    ASSERT_EQ(components.size(), sizeof(InsertCmdShapeComponents));
}

// Test that inserts with different document sizes have the same component size (documents not
// stored).
TEST_F(InsertCmdShapeTest, ShapeComponentsSizeDoesNotVaryWithDocumentSize) {
    auto icr1 = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testcoll",
        documents: [ { a: 1 } ],
        "$db": "testdb"
    })"sv));
    auto icr2 = InsertCommandRequest::parseOwned(fromjson(R"({
        insert: "testcoll",
        documents: [ { a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8 } ],
        "$db": "testdb"
    })"sv));

    InsertCmdShape shape1(icr1);
    InsertCmdShape shape2(icr2);

    ASSERT_EQ(static_cast<const InsertCmdShapeComponents&>(shape1.specificComponents()).size(),
              static_cast<const InsertCmdShapeComponents&>(shape2.specificComponents()).size());
}

// Verifies that "insert" command shape hash value is stable (doesn't change between
// the versions of the server).
TEST_F(InsertCmdShapeTest, StableQueryShapeHashValue) {
    auto hash = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");

    std::string expectedHash = "AC5B6ED44B30138504E115AEFAF6126598B912F96CE6F3AF75943205260DE0AC";
    ASSERT_EQ(expectedHash, hash.toHexString());

    // Changing the documents should not change the hash.
    hash = makeShapeHash(R"({
        insert: "testcoll",
        documents: [ { x: 1 }, { y: 1 }, { z: 1 } ],
        "$db": "testdb"
    })");
    ASSERT_EQ(expectedHash, hash.toHexString());

    // Changing the collection should change the hash.
    hash = makeShapeHash(R"({
        insert: "testcoll2",
        documents: [ { x: 1 } ],
        "$db": "testdb"
    })");
    expectedHash = "7D1781C3B954CFED13EC6E3114688A2D7397FA5E6ED9F3EB083C97CB3BDC7B11";
    ASSERT_EQ(expectedHash, hash.toHexString());
}

}  // namespace
}  // namespace mongo::query_shape
