// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/delete_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::query_shape {
namespace {
using namespace std::literals::string_view_literals;

using write_ops::DeleteCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

struct DeleteCmdBuilder {
    std::string database;
    std::string collection;
    int limit;
    BSONObj q;
    BSONObj let = BSONObj();
    BSONObj collation = BSONObj();

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("delete", collection);
        BSONArrayBuilder deletes(builder.subarrayStart("deletes"));
        BSONObjBuilder deleteObj;

        deleteObj.append("q", q);
        deleteObj.append("limit", limit);

        if (!collation.isEmpty()) {
            deleteObj.append("collation", collation);
        }

        deletes.append(deleteObj.obj());
        deletes.done();
        if (!let.isEmpty()) {
            builder.append("let", let);
        }
        builder.append("$db", database);
        return builder.obj();
    }
};

class DeleteCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    DeleteCmdShape makeOneShapeFromDelete(std::string_view deleteCmd) {
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
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    // No literals or field names in an empty query, so all three opts produce the same output.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: {},
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, SingleDocumentDeleteShapeWithQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 1 } ],
        "$db": "testDB" 
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: 1 } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: "?number" } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<x>": { $eq: "?number" } },
            limit: 1 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, MultiDocumentDeleteShapeWithQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB" 
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: 1 } },
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { x: { $eq: "?number" } },
            limit: 0})",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<x>": { $eq: "?number" } },
            limit: 0 })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, ComplexDeleteQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { $and: [ { age: { $gt: 18 } }, { status: "active" } ] }, limit: 0 } ],
        "$db": "testDB" 
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { $and: [ { age: { $gt: 1 } }, { status: { $eq: "?" } } ] },
            limit: 0 
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { $and: [ { age: { $gt: "?number" } }, { status: { $eq: "?string" } } ] },
            limit: 0
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { $and: [ { "HASH<age>": { $gt: "?number" } }, { "HASH<status>": { $eq: "?string" } }
            ] }, limit: 0
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, DeleteWithIdQuery) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { _id: 5 }, limit: 1 } ],
        "$db": "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { _id: { $eq: 1 } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "delete",
            q: { _id: { $eq: "?number" } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "delete",
            q: { "HASH<_id>": { $eq: "?number" } },
            limit: 1
        })",
        shape.toBson(_operationContext.get(),
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
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
    })"sv);

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
                     query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
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
                     query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
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
                     query_shape::SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
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
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
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
                         query_shape::SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(DeleteCmdShapeTest, SizeCalculation) {
    auto shape = makeOneShapeFromDelete(R"({
        delete: "testColl",
        deletes: [ { q: { x: 1 }, limit: 0 } ],
        "$db": "testDB"
    })"sv);

    const auto& components =
        static_cast<const DeleteCmdShapeComponents&>(shape.specificComponents());

    ASSERT_EQ(components.size(),
              sizeof(DeleteCmdShapeComponents) + components.representativeQ.objsize() +
                  components.let.size() - sizeof(LetShapeComponent));
}

// Verifies that "delete" command shape hash value is stable (does not change between the
// versions of the server).
TEST_F(DeleteCmdShapeTest, StableQueryShapeHashValue) {
    DeleteCmdBuilder deleteCmd;
    deleteCmd.database = "testDB";
    deleteCmd.collection = "testColl";
    deleteCmd.q = BSON("z" << BSON("$eq" << 3));
    deleteCmd.limit = 0;
    auto serializationContext = SerializationContext::stateCommandRequest();

    auto verifyHash = [&](std::string_view expectedHash, const DeleteCmdBuilder& deleteCmd) {
        auto shapes = makeShapesFromDelete(deleteCmd.toBSON());
        ASSERT_EQ(shapes.size(), 1);
        const DeleteCmdShape& shape = shapes.front();

        const auto hash = shape.sha256Hash(_operationContext.get(), serializationContext);
        ASSERT_EQ(expectedHash, hash.toHexString());
    };

    std::string expectedHash = "6DF604FDD3B470BA42E917DBD19A00E6F4A44F75155D71A93887C7D14A202D00";
    verifyHash(expectedHash, deleteCmd);

    // Changing the literal value in the query should not change the hash.
    deleteCmd.q = BSON("z" << BSON("$eq" << 10));
    verifyHash(expectedHash, deleteCmd);

    // Changing the field name in the query should change the hash.
    deleteCmd.q = BSON("x" << BSON("$eq" << 10));
    expectedHash = "62480E64F443EC8067E8B39BCAC593450560A6B2E67E6E9520C325685E2278C4";
    verifyHash(expectedHash, deleteCmd);

    // Changing the limit value should change the hash.
    deleteCmd.limit = 1;
    expectedHash = "8CA2D5538A474668A9FF524038511397BC5986BFC6FEF8850FBB3329B0C0059F";
    verifyHash(expectedHash, deleteCmd);

    // Adding a let should change the hash.
    deleteCmd.let = BSON("myVar" << 1);
    expectedHash = "0177F690503D6F94C2F52E0B140BF8C82029C8A354B383FE6D2922DC2086389D";
    verifyHash(expectedHash, deleteCmd);

    // Changing a literal value in the let should not change the hash.
    deleteCmd.let = BSON("myVar" << 10);
    verifyHash(expectedHash, deleteCmd);

    // Changing the type of field in the let should change the hash.
    deleteCmd.let = BSON("myVar" << "hello");
    expectedHash = "E90337396A2EDE0D5C287A2824F3708C3C5E3E450764DC87871C9920C8844794";
    verifyHash(expectedHash, deleteCmd);

    // Setting a collation should change the hash.
    deleteCmd.collation = BSON("locale" << "fr");
    expectedHash = "1776AF4E1352F9545889D579488687ACA6B38B17033AA08FD004B1142FD39348";
    verifyHash(expectedHash, deleteCmd);

    // Changing the collection should change the hash.
    deleteCmd.collection = "testColl2";
    expectedHash = "D2A9577D063A2721593452E098ADF8837D451AEA7A4DA132C0E3DE78F0D0EDA5";
    verifyHash(expectedHash, deleteCmd);
}

TEST_F(DeleteCmdShapeTest, EmptyLetNoLetHaveSameHash) {
    auto shapeWithEmptyLet = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0 } ],
       let: {},
       "$db": "testDB"
   })");
    auto shapeWithoutLet = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0 } ],
       "$db": "testDB"
    })");

    ASSERT_EQ(shapeWithEmptyLet.sha256Hash(_operationContext.get(), {}),
              shapeWithoutLet.sha256Hash(_operationContext.get(), {}));
}

TEST_F(DeleteCmdShapeTest, EmptyHintNoHintHaveSameHash) {
    auto shapeWithEmptyHint = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0, hint: {}} ],
       "$db": "testDB"
   })");
    auto shapeWithoutHint = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0 } ],
       "$db": "testDB"
   })");


    ASSERT_EQ(shapeWithEmptyHint.sha256Hash(_operationContext.get(), {}),
              shapeWithoutHint.sha256Hash(_operationContext.get(), {}));
}

TEST_F(DeleteCmdShapeTest, EmptyCollationNoCollationHaveSameHash) {
    auto shapeWithEmptyCollation = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0, collation: {}} ],
       "$db": "testDB"
   })");
    auto shapeWithoutCollation = makeOneShapeFromDelete(R"({
       delete: "testColl",
       deletes: [ { q: { x: 1 }, limit: 0 } ],
       "$db": "testDB"
   })");


    ASSERT_EQ(shapeWithEmptyCollation.sha256Hash(_operationContext.get(), {}),
              shapeWithoutCollation.sha256Hash(_operationContext.get(), {}));
}

}  // namespace
}  // namespace mongo::query_shape
