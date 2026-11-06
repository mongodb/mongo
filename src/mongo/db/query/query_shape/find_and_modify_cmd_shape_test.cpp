// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_shape/find_and_modify_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/delete_util.h"
#include "mongo/db/query/write_ops/parsed_find_and_modify.h"
#include "mongo/db/update/update_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string_view>
#include <utility>

namespace mongo::query_shape {
namespace {

using namespace std::literals::string_view_literals;

using write_ops::FindAndModifyCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class FindAndModifyCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
    }

    boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
        const FindAndModifyCommandRequest& request) {
        auto runtimeConstants = request.getLegacyRuntimeConstants()
            ? request.getLegacyRuntimeConstants()
            : boost::optional<LegacyRuntimeConstants>{
                  Variables::generateRuntimeConstants(_operationContext.get())};
        return ExpressionContextBuilder{}
            .opCtx(_operationContext.get())
            .ns(request.getNamespace())
            .letParameters(request.getLet())
            .runtimeConstants(std::move(runtimeConstants))
            .build();
    }

    FindAndModifyCmdShape makeOneShape(std::string_view cmd) {
        auto request = FindAndModifyCommandRequest::parseOwned(fromjson(cmd));
        auto expCtx = makeExpressionContext(request);
        if (request.getRemove().value_or(false)) {
            DeleteRequest deleteRequest;
            deleteRequest.setNsString(request.getNamespace());
            delete_util::makeDeleteRequest(_operationContext.get(), request, false, &deleteRequest);
            auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
                expCtx,
                &request,
                &deleteRequest,
                makeExtensionsCallback<ExtensionsCallbackNoop>()));
            return FindAndModifyCmdShape(request, *parsed.getParsedDelete(), expCtx);
        }
        UpdateRequest updateRequest;
        updateRequest.setNamespaceString(request.getNamespace());
        update::makeUpdateRequest(
            _operationContext.get(), request, boost::none /*explain*/, &updateRequest);
        auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
            expCtx, &request, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));
        return FindAndModifyCmdShape(request, *parsed.getParsedUpdate(), expCtx);
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
};

TEST_F(FindAndModifyCmdShapeTest, BasicUpdateFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: {$eq: 3}},
        update: {$set: {item: "ABC"}},
        new: true,
        upsert: true,
        $db: "testDB"
    })"sv);

    // 'new' is part of the query stats key rather than the shape, so it must not appear here.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: 1 } },
            update: { $set: { item: "?" } },
            upsert: true
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: "?number" } },
            update: { $set: { item: "?string" } },
            upsert: true
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, RemoveFindAndModifyShapeWithSortAndFields) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        sort: {b: 1},
        fields: {e: 1},
        remove: true,
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { a: { $eq: 1 } },
            sort: { b: 1 },
            fields: { e: true, _id: true },
            remove: true
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { a: { $eq: "?number" } },
            sort: { b: 1 },
            fields: { e: true, _id: true },
            remove: "?bool"
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

// An explicit 'remove: false' runs the update branch, so the shape must not carry a 'remove'
// field, and re-shapifying must not confuse the shapified 'remove' output with the branch.
TEST_F(FindAndModifyCmdShapeTest, ExplicitRemoveFalseIsUpdateBranch) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: {$eq: 3}},
        update: {$set: {item: "ABC"}},
        remove: false,
        $db: "testDB"
    })"sv);

    auto components =
        static_cast<const FindAndModifyCmdShapeComponents&>(shape.specificComponents());
    ASSERT(!components.isRemove);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: 1 } },
            update: { $set: { item: "?" } },
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, UpsertIsKeptUnmodifiedInShape) {
    auto upsertShape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        upsert: true,
        $db: "testDB"
    })"sv);
    auto noUpsertShape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        upsert: false,
        $db: "testDB"
    })"sv);

    // The boolean is not redacted, even in debug output, and the two values produce different
    // shapes.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: "?number" } },
            update: { $set: { item: "?string" } },
            upsert: true
        })",
        upsertShape.toBson(_operationContext.get(),
                           SerializationOptions::kDebugQueryShapeSerializeOptions,
                           SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: "?number" } },
            update: { $set: { item: "?string" } },
            upsert: false
        })",
        noUpsertShape.toBson(_operationContext.get(),
                             SerializationOptions::kDebugQueryShapeSerializeOptions,
                             SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, NewDoesNotAffectShape) {
    auto newTrue = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: true,
        $db: "testDB"
    })"sv);
    auto newFalse = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        new: false,
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ(
        newTrue.toBson(_operationContext.get(),
                       SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                       SerializationContext::stateDefault()),
        newFalse.toBson(_operationContext.get(),
                        SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                        SerializationContext::stateDefault()));
    ASSERT_EQ(newTrue.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()),
              newFalse.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, HintDoesNotAffectShape) {
    auto withHint = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        hint: {x: 1},
        $db: "testDB"
    })"sv);
    auto withoutHint = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ(
        withHint.toBson(_operationContext.get(),
                        SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                        SerializationContext::stateDefault()),
        withoutHint.toBson(_operationContext.get(),
                           SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                           SerializationContext::stateDefault()));
}

// A $natural sort is really a hint: it must not appear in the shape.
TEST_F(FindAndModifyCmdShapeTest, NaturalSortIsHintNotShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        sort: {$natural: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { a: { $eq: 1 } },
            update: { $set: { item: "?" } },
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, SimpleIdQueryFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { _id: { $eq: 1 } },
            update: { $set: { item: "?" } },
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, PipelineUpdateFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: [{$set: {foo: "bar", num: 42}}],
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: 1 } },
            update: [ { $set: { foo: { $const: "?" }, num: { $const: 1 } } } ],
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, NoopModifierFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$inc: {}},
        $db: "testDB"
    })"sv);

    // An all-no-op modifier update must not collapse into a replacement update.
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: 1 } },
            update: { $inc: {} },
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, ArrayFiltersFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {"grades.$[g].score": 5}},
        arrayFilters: [{'g.kind': "ok"}],
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { x: { $eq: 1 } },
            update: { $set: { "grades.$[g].score": 1 } },
            arrayFilters: [ { "g.kind": { $eq: "?" } } ],
            upsert: false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, LetFindAndModifyShape) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: { $expr: { $gte: ["$x", "$$threshold"] } },
        update: {$set: {item: "ABC"}},
        let: {threshold: 10},
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "testDB", coll: "testColl" },
            command: "findAndModify",
            query: { $expr: { $gte: [ "$x", "$$threshold" ] } },
            update: { $set: { item: "?" } },
            upsert: false,
            let: { threshold: { $const: 1 } }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

// The debug and identifier-transformed shapes are computed by re-parsing the stored
// representative shapes, so they must round-trip through the representative serialization.
TEST_F(FindAndModifyCmdShapeTest, ReshapesFromRepresentativeComponents) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5, b: "hello"},
        sort: {s: -1},
        update: {$set: {"grades.$[g].score": 5}, $inc: {n: 3}},
        arrayFilters: [{'g.kind': "ok"}],
        upsert: true,
        let: {threshold: 10},
        $db: "testDB"
    })"sv);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" },
            command: "findAndModify",
            query: { $and: [ { "HASH<a>": { $eq: "?number" } }, { "HASH<b>": { $eq: "?string" } } ] },
            sort: { "HASH<s>": -1 },
            update: { $inc: { "HASH<n>": "?number" }, $set: { "HASH<grades>.$[HASH<g>].HASH<score>": "?number" } },
            arrayFilters: [ { "HASH<g>.HASH<kind>": { $eq: "?string" } } ],
            upsert: true,
            let: { "HASH<threshold>": "?number" }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, ShapeHashStabilityAndDifferentiation) {
    auto shape1 = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);
    auto shape2 = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1000},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);
    auto otherQuery = makeOneShape(R"({
        findAndModify: "testColl",
        query: {y: 1},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);
    auto otherUpsert = makeOneShape(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {item: "ABC"}},
        upsert: true,
        $db: "testDB"
    })"sv);

    // Different literals collapse to the same shape and hash.
    ASSERT_BSONOBJ_EQ(shape1.toBson(_operationContext.get(),
                                    SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                    SerializationContext::stateDefault()),
                      shape2.toBson(_operationContext.get(),
                                    SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                                    SerializationContext::stateDefault()));
    ASSERT_EQ(shape1.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()),
              shape2.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()));

    // A different query and a different upsert produce different hashes.
    ASSERT_NE(shape1.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()),
              otherQuery.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()));
    ASSERT_NE(
        shape1.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()),
        otherUpsert.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()));
}

void validateShapeSize(const FindAndModifyCmdShape& shape) {
    auto components =
        static_cast<const FindAndModifyCmdShapeComponents&>(shape.specificComponents());

    const auto arrayFiltersSize = components.representativeArrayFilters
        ? shape_helpers::containerSize(*components.representativeArrayFilters)
        : 0;

    ASSERT_EQ(components.size(),
              sizeof(FindAndModifyCmdShapeComponents) + components.representativeQuery.objsize() +
                  components.representativeSort.objsize() +
                  components.representativeFields.objsize() +
                  components._representativeUpdateObj.objsize() + arrayFiltersSize +
                  components.let.size() - sizeof(LetShapeComponent));
}

TEST_F(FindAndModifyCmdShapeTest, ShapeSizeAccounting) {
    auto updateShape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        sort: {b: 1},
        update: {$set: {item: "ABC"}},
        let: {threshold: 10},
        $db: "testDB"
    })"sv);
    validateShapeSize(updateShape);

    auto removeShape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {e: 1},
        remove: true,
        $db: "testDB"
    })"sv);
    validateShapeSize(removeShape);
}

// Building the shape must reuse the projection already parsed and cached by ParsedFindAndModify.
// Otherwise the shape construction parses 'fields' again and charges the memory-intensive
// expression budget twice, which fails when the limit is reached.
TEST_F(FindAndModifyCmdShapeTest, ReusesCachedProjectionForUpdate) {
    unittest::ServerParameterGuard memoryIntensiveExprLimit(
        "internalQueryMaxMemoryIntensiveExpressions", 1);

    auto request = FindAndModifyCommandRequest::parseOwned(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$a", "$b"]}},
        update: {$set: {updated: true}},
        $db: "testDB"
    })"));
    auto expCtx = makeExpressionContext(request);

    UpdateRequest updateRequest;
    updateRequest.setNamespaceString(request.getNamespace());
    update::makeUpdateRequest(
        _operationContext.get(), request, boost::none /*explain*/, &updateRequest);
    auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
        expCtx, &request, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    // Warm the projection cache. This charges the budget once.
    ASSERT_FALSE(
        parsed.serializeProjection(SerializationOptions::kRepresentativeQueryShapeSerializeOptions)
            .isEmpty());

    // Building and hashing the shape must reuse the cache rather than reparsing.
    FindAndModifyCmdShape shape(parsed, expCtx);
    const auto shapeBson =
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault());
    ASSERT_TRUE(shapeBson.hasField("fields"));
    std::ignore = shape.sha256Hash(_operationContext.get(), SerializationContext::stateDefault());
}

TEST_F(FindAndModifyCmdShapeTest, ReusesCachedProjectionForRemove) {
    unittest::ServerParameterGuard memoryIntensiveExprLimit(
        "internalQueryMaxMemoryIntensiveExpressions", 1);

    auto request = FindAndModifyCommandRequest::parseOwned(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$a", "$b"]}},
        remove: true,
        $db: "testDB"
    })"));
    auto expCtx = makeExpressionContext(request);

    DeleteRequest deleteRequest;
    deleteRequest.setNsString(request.getNamespace());
    delete_util::makeDeleteRequest(_operationContext.get(), request, false, &deleteRequest);
    auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
        expCtx, &request, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    // Warm the projection cache. This charges the budget once.
    ASSERT_FALSE(
        parsed.serializeProjection(SerializationOptions::kRepresentativeQueryShapeSerializeOptions)
            .isEmpty());

    // Building and hashing the shape must reuse the cache rather than reparsing.
    FindAndModifyCmdShape shape(parsed, expCtx);
    const auto shapeBson =
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault());
    ASSERT_TRUE(shapeBson.hasField("fields"));
    std::ignore = shape.sha256Hash(_operationContext.get(), SerializationContext::stateDefault());
}

TEST_F(FindAndModifyCmdShapeTest, AbsentAndEmptyFieldsProduceSameShape) {
    auto absentFields = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);
    auto emptyFields = makeOneShape(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {},
        update: {$set: {item: "ABC"}},
        $db: "testDB"
    })"sv);

    const auto absentBson =
        absentFields.toBson(_operationContext.get(),
                            SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                            SerializationContext::stateDefault());
    const auto emptyBson =
        emptyFields.toBson(_operationContext.get(),
                           SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                           SerializationContext::stateDefault());
    ASSERT_FALSE(absentBson.hasField("fields"));
    ASSERT_FALSE(emptyBson.hasField("fields"));
    ASSERT_BSONOBJ_EQ(absentBson, emptyBson);
    ASSERT_EQ(
        absentFields.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()),
        emptyFields.sha256Hash(_operationContext.get(), SerializationContext::stateDefault()));
}

TEST_F(FindAndModifyCmdShapeTest, EmptyFieldsOnIdFastPathAreNotSerialized) {
    auto shape = makeOneShape(R"({
        findAndModify: "testColl",
        query: {_id: 1},
        fields: {},
        remove: true,
        $db: "testDB"
    })"sv);

    auto components =
        static_cast<const FindAndModifyCmdShapeComponents&>(shape.specificComponents());
    ASSERT_TRUE(components.representativeFields.isEmpty());
    ASSERT_FALSE(shape
                     .toBson(_operationContext.get(),
                             SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                             SerializationContext::stateDefault())
                     .hasField("fields"));
}

// On mongod, a rawData write to a legacy viewful timeseries view is executed against the buckets
// collection, so the per-op request nss differs from the command nss. Like the update and delete
// shapes, the findAndModify shape must use the request's namespace, which is what the write
// actually runs against.
TEST_F(FindAndModifyCmdShapeTest, ShapeNamespaceComesFromParsedRequestForUpdate) {
    auto request = FindAndModifyCommandRequest::parseOwned(fromjson(R"({
        findAndModify: "testColl",
        query: {x: 1},
        update: {$set: {y: 1}},
        $db: "testDB"
    })"));
    auto expCtx = makeExpressionContext(request);
    const auto bucketsNss =
        NamespaceString::createNamespaceString_forTest("testDB.system.buckets.testColl");

    UpdateRequest updateRequest;
    updateRequest.setNamespaceString(bucketsNss);
    update::makeUpdateRequest(
        _operationContext.get(), request, boost::none /*explain*/, &updateRequest);
    auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
        expCtx, &request, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    FindAndModifyCmdShape branchShape(request, *parsed.getParsedUpdate(), expCtx);
    ASSERT_EQ(bucketsNss, branchShape.nssOrUUID.nss());

    FindAndModifyCmdShape wrapperShape(parsed, expCtx);
    ASSERT_EQ(bucketsNss, wrapperShape.nssOrUUID.nss());
}

TEST_F(FindAndModifyCmdShapeTest, ShapeNamespaceComesFromParsedRequestForRemove) {
    auto request = FindAndModifyCommandRequest::parseOwned(fromjson(R"({
        findAndModify: "testColl",
        query: {x: 1},
        remove: true,
        $db: "testDB"
    })"));
    auto expCtx = makeExpressionContext(request);
    const auto bucketsNss =
        NamespaceString::createNamespaceString_forTest("testDB.system.buckets.testColl");

    DeleteRequest deleteRequest;
    deleteRequest.setNsString(bucketsNss);
    delete_util::makeDeleteRequest(_operationContext.get(), request, false, &deleteRequest);
    auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
        expCtx, &request, &deleteRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    FindAndModifyCmdShape branchShape(request, *parsed.getParsedDelete(), expCtx);
    ASSERT_EQ(bucketsNss, branchShape.nssOrUUID.nss());

    FindAndModifyCmdShape wrapperShape(parsed, expCtx);
    ASSERT_EQ(bucketsNss, wrapperShape.nssOrUUID.nss());
}

}  // namespace
}  // namespace mongo::query_shape
