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

#include "mongo/db/query/query_shape/delete_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {
namespace {

using write_ops::DeleteCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class DeleteCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    DeleteCmdShape makeOneShapeFromDelete(StringData deleteCmd) {
        auto shapes = makeShapesFromDelete(fromjson(deleteCmd));
        ASSERT_EQ(shapes.size(), 1U);
        return shapes.front();
    }

    std::vector<DeleteCmdShape> makeShapesFromDelete(BSONObj deleteCmd) {
        auto deleteRequest = DeleteCommandRequest::parseOwned(std::move(deleteCmd));
        return makeShapesFromDeleteRequest(deleteRequest);
    }

    std::vector<DeleteCmdShape> makeShapesFromDeleteRequest(
        const DeleteCommandRequest& deleteRequest) {
        std::vector<DeleteCmdShape> shapes;
        for (const auto& op : deleteRequest.getDeletes()) {
            DeleteRequest request;
            request.setNsString(kDefaultTestNss);
            request.setQuery(op.getQ());
            request.setMulti(op.getMulti());
            if (op.getCollation()) {
                request.setCollation(op.getCollation()->getOwned());
            }
            if (deleteRequest.getLet()) {
                request.setLet(*deleteRequest.getLet());
            }

            _expCtx = makeBlankExpressionContext(
                _operationContext.get(), deleteRequest.getNamespace(), deleteRequest.getLet());

            auto parsedDelete = uassertStatusOK(parsed_delete_command::parse(
                _expCtx, &request, makeExtensionsCallback<ExtensionsCallbackNoop>()));
            shapes.emplace_back(deleteRequest, parsedDelete, _expCtx);
        }
        return shapes;
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;
    ServiceContext::UniqueOperationContext _operationContext;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(DeleteCmdShapeTest, EmptyQueryDeleteShape) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: {}, limit: 0 } ],
        "$db": "testDB" 
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    // No literals or field names in an empty query, so all three opts produce the same output.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, SingleDocumentDeleteShapeWithQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 1 } ],
        "$db": "testDB" 
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: 1 } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: "?number" } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<x>": { $eq: "?number" } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, MultiDocumentDeleteShapeWithQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB" 
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: 1 } },
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: "?number" } },
            limit: 0})",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<x>": { $eq: "?number" } },
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, ComplexDeleteQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { $and: [ { age: { $gt: 18 } }, { status: "active" } ] }, limit: 0 } ],
        "$db": "testDB" 
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { $and: [ { age: { $gt: 1 } }, { status: { $eq: "?" } } ] },
            limit: 0 
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { $and: [ { age: { $gt: "?number" } }, { status: { $eq: "?string" } } ] },
            limit: 0
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { $and: [ { "HASH<age>": { $gt: "?number" } }, { "HASH<status>": { $eq: "?string" } }
            ] }, limit: 0
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, DeleteWithIdQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { _id: 5 }, limit: 1 } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { _id: { $eq: 1 } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { _id: { $eq: "?number" } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<_id>": { $eq: "?number" } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

// Tests all possible delete shape fields (q, limit, collation, let) under all three
// serialization options in one place. Collation is never shapified; q, limit, and let
// have their literals redacted according to the opts.
TEST_F(DeleteCmdShapeTest, AllDeleteFieldsAllSerializationOptions) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { $expr: { $eq: ["$x", "$$myVar"] } }, limit: 1, collation: { locale: "en", strength: 2 } } ],
        let: { myVar: "hello" },
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            collation: { locale: "en", strength: 2 },
            command: "delete",
            q: { $expr: { $eq: [ "$x", "$$myVar" ] } },
            limit: 1,
            let: { myVar: { $const: "?" } }
    })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            collation: { locale: "en", strength: 2 },
            command: "delete",
            q: { $expr: { $eq: [ "$x", "$$myVar" ] } },
            limit: 1,
            let: { myVar: "?string" }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            collation: { locale: "en", strength: 2 },
            command: "delete",
            q: { $expr: { $eq: [ "$HASH<x>", "$$HASH<myVar>" ] } },
            limit: 1,
            let: { "HASH<myVar>": "?string" }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, MultipleDeletesProduceSeparateShapes) {
    // Verifies the let is propagated into each delete shape correctly.
    auto shapes = makeShapesFromDelete(fromjson(R"({
        delete: "testColl",
        deletes: [
            { q: { x: 1 }, limit: 1 },
            { q: { y: {$gt: 2 }, z: "foo" }, limit: 0 }
        ],
        let: { myVar: 42 },
        "$db": "testDB"
    })"));
    ASSERT_EQ(shapes.size(), 2);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: "?number" } },
            limit: 1,
            let: { myVar: "?number" }
        })",
        shapes[0].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            "q": {
                "$and": [
                    {
                        "y": {
                            "$gt": "?number"
                        }
                    },
                    {
                        "z": {
                            "$eq": "?string"
                        }
                    }
                ]
            },
            limit: 0,
            let: { myVar: "?number" }
        })",
        shapes[1].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, SizeCalculation) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"_sd);

    const auto& components =
        static_cast<const DeleteCmdShapeComponents&>(shape.specificComponents());

    ASSERT_EQ(components.size(),
              sizeof(DeleteCmdShapeComponents) + components.representativeQ.objsize() +
                  components.let.size() - sizeof(LetShapeComponent));
}

}  // namespace
}  // namespace mongo::query_shape
