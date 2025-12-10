/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/update_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/update_cmd_builder.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {
namespace {

using write_ops::UpdateCommandRequest;

static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class UpdateCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::vector<UpdateCmdShape> makeShapesFromUpdate(StringData updateCmd) {
        auto updateRequest = UpdateCommandRequest::parseOwned(fromjson(updateCmd));
        return makeShapesFromUpdateRequest(updateRequest);
    }

    std::vector<UpdateCmdShape> makeShapesFromUpdateRequest(
        const write_ops::UpdateCommandRequest& updateRequest) {
        std::vector<UpdateCmdShape> shapes;
        for (const auto& op : updateRequest.getUpdates()) {
            UpdateRequest request(op);
            request.setNamespaceString(kDefaultTestNss);
            if (updateRequest.getLet()) {
                request.setLetParameters(*updateRequest.getLet());
            }

            _expCtx = makeBlankExpressionContext(
                _operationContext.get(), updateRequest.getNamespace(), updateRequest.getLet());

            auto parsedUpdate = uassertStatusOK(
                parsed_update_command::parse(_expCtx,
                                             &request,
                                             makeExtensionsCallback<ExtensionsCallbackReal>(
                                                 _operationContext.get(), &request.getNsString())));
            shapes.emplace_back(updateRequest, parsedUpdate, _expCtx);
        }
        return shapes;
    }

    UpdateCmdShape makeOneShapeFromUpdate(StringData updateCmd) {
        auto shapes = makeShapesFromUpdate(updateCmd);
        ASSERT_EQ(shapes.size(), 1);
        return shapes.front();
    }

    void validateShapeSize(const UpdateCmdShape& shape) {
        auto updateComponents =
            static_cast<const UpdateCmdShapeComponents&>(shape.specificComponents());

        const auto letSize = updateComponents.let.size();
        const auto cSize =
            (updateComponents.representativeC) ? updateComponents.representativeC->objsize() : 0;

        ASSERT_EQ(updateComponents.size(),
                  sizeof(UpdateCmdShapeComponents) + updateComponents.representativeQ.objsize() +
                      updateComponents._representativeUObj.objsize() + cSize + letSize -
                      sizeof(LetShapeComponent));
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(UpdateCmdShapeTest, BasicReplacementUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": false 
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": {
                "?": "?"
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BasicModifierUpdateShape) {
    // Note that $set and $setOnInsert uses the same SetNode implementation.
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: { 
                    $set: {
                        item: "ABC123",
                        "info.publisher": "2222",
                        tags: [ "software" ],
                        "ratings.1": { by: "xyz", rating: 3 },
                        emptyField: "",
                        emptyObject: {}
                    },
                    $setOnInsert: { newInsert: true },
                    $set: {},
                    $setOnInsert: {}
                }, 
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { 
                x: { $eq: 1 } 
            }, 
            u: {
                $set: { 
                    emptyField: "?", 
                    emptyObject: { "?": "?" }, 
                    "info.publisher": "?", 
                    item: "?", 
                    "ratings.1": { "?": "?" }, 
                    tags: [ "?" ] 
                },
                $setOnInsert: { newInsert: true } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: {
                $set: { 
                    emptyField: "?string",
                    emptyObject: "?object", 
                    "info.publisher": "?string", 
                    item: "?string", 
                    "ratings.1": "?object", 
                    tags: "?array<?string>" 
                },
                $setOnInsert: { newInsert: "?bool" }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "HASH<testDB>", 
                coll: "HASH<testColl>" 
            }, 
            command: "update", 
            q: { "HASH<x>": { $eq: "?number" } }, 
            u: {
                $set: { 
                    "HASH<emptyField>": "?string",
                    "HASH<emptyObject>": "?object", 
                    "HASH<info>.HASH<publisher>": "?string", 
                    "HASH<item>": "?string", 
                    "HASH<ratings>.HASH<1>": "?object", 
                    "HASH<tags>": "?array<?string>" 
                },
                $setOnInsert: { "HASH<newInsert>": "?bool" }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BasicPipelineUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: [ { "$set": { "foo": "bar", "num": 42 } } ], multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": [
                {
                    "$set": {
                        "foo": "?string",
                        "num": "?number"
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                        "$eq": 1
                    }
            },
            "u": [
                {
                    "$set": {
                        "foo": {
                            "$const": "?"
                        },
                        "num": {
                            "$const": 1
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, CurrentDateModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: { 
                    $currentDate: { lastModified: { $type: "timestamp" } },
                    $currentDate: { firstModified: { $type: "date" } },
                    $currentDate: { example: true },
                    $currentDate: { exampleTimestamp: false },
                    $currentDate: {}
                }, 
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { 
                x: { $eq: 1 } 
            }, 
            u: {
                $currentDate: { 
                    example: { $type: "date" }, 
                    exampleTimestamp: { $type: "date" }, 
                    firstModified: { $type: "date" }, 
                    lastModified: { $type: "timestamp" } 
                }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: {
                $currentDate: { 
                    example: { $type: "date" }, 
                    exampleTimestamp: { $type: "date" }, 
                    firstModified: { $type: "date" }, 
                    lastModified: { $type: "timestamp" } 
                }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
        cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } }, 
        u: {
            $currentDate: { 
                "HASH<example>": { $type: "date" }, 
                "HASH<exampleTimestamp>": { $type: "date" }, 
                "HASH<firstModified>": { $type: "date" }, 
                "HASH<lastModified>": { $type: "timestamp" } 
            }
        }, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

/**
 * Test that arithmetic-related modifier update operators are tested properly.
 * Note that both $inc and $mul both use ArithmeticNode.
 */
TEST_F(UpdateCmdShapeTest, ArithmeticModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: { 
                    $mul: { amount: 2 },
                    $inc: { quantity: -2, "metrics.orders": 1 },
                    $mul: {},
                    $inc: {}  
                },
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: { 
                $inc: { "metrics.orders": 1, quantity: 1 }, 
                $mul: { amount: 1 } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: { 
                $inc: { "metrics.orders": "?number", quantity: "?number" },
                $mul: { amount: "?number" } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
        cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } }, 
        u: { 
            $inc: { "HASH<metrics>.HASH<orders>": "?number", "HASH<quantity>": "?number" },
            $mul: { "HASH<amount>": "?number" } 
        }, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

/**
 * Test that compare-related modifier update operators are tested properly.
 * $max and $mul both are instances of CompareNode.
 */
TEST_F(UpdateCmdShapeTest, CompareModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: {
                    $min: { minPrice: 5 },
                    $max: { maxPrice: 500 },
                    $min: {},
                    $max: {}
                },
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: {
                $max: { maxPrice: 1 }, 
                $min: { minPrice: 1 }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: {
                $max: { maxPrice: "?number" }, 
                $min: { minPrice: "?number" }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
        cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } }, 
        u: {
            $max: { "HASH<maxPrice>": "?number" }, 
            $min: { "HASH<minPrice>": "?number" }
        }, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, AddToSetModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{ 
                q: { x: {$eq: 3} }, 
                u: {
                    $addToSet: { testEmpty: { $each: [] } }, 
                    $addToSet: { scores: { $each: [ 50, 60, 70 ] } }, 
                    $addToSet: { scoresSingleAdd: 50 },
                    $addToSet: {}
                }, 
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { 
                x: { $eq: 1 } 
            }, 
            u: {
                $addToSet: { 
                    scores: { $each: [ 1 ] }, 
                    scoresSingleAdd: 1, 
                    testEmpty: { $each: [] }
                }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { 
                db: "testDB", 
                coll: "testColl" 
            }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: {
                $addToSet: { 
                    scores: { $each: "?array<?number>" }, 
                    scoresSingleAdd: "?number", 
                    testEmpty: { $each: "[]" } 
                }
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
        cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } }, 
        u: {
            $addToSet: { 
                "HASH<scores>": { $each: "?array<?number>" }, 
                "HASH<scoresSingleAdd>": "?number", 
                "HASH<testEmpty>": { $each: "[]" } 
            }
        }, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, PushModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: {
                    $push: { scoresSinglePush: 89 },
                    $push: { tests: { $each: [ 40, 60 ], $sort: -1 } },
                    $push: { testsEmpty: { $each: [ ], $sort: 1 } },
                    $push: {
                        scoresWithPostion: {
                            $each: [ 50, 60, 70 ],
                            $position: 0
                        }
                    },
                    $push: {
                        scoresToSlice: {
                            $each: [ 80, 78, 86 ],
                            $slice: -5
                        }
                    },
                    $push: {
                        quizzes: {
                            $each: [ { id: 3, score: 8 }, { id: 4, score: 7 }, { id: 5, score: 6 } ],
                            $sort: { score: 1, name: -1 }
                        }
                    },
                    $push: {}
                },
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: {
                $push: { 
                    quizzes: { $each: [ { "?": "?" } ], $sort: { score: 1, name: -1 } }, 
                    scoresSinglePush: { $each: [ 1 ] }, 
                    scoresToSlice: { $each: [ 1 ], $slice: 1 }, 
                    scoresWithPostion: { $each: [ 1 ], $position: 1 }, 
                    tests: { $each: [ 1 ], $sort: -1 }, 
                    testsEmpty: { $each: [], $sort: 1 }
                } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: "?number" } }, 
            u: {
                $push: { 
                    quizzes: { $each: "?array<?object>", $sort: { score: 1, name: -1 } }, 
                    scoresSinglePush: { $each: "?array<?number>" }, 
                    scoresToSlice: { $each: "?array<?number>", $slice: "?number" }, 
                    scoresWithPostion: { $each: "?array<?number>", $position: "?number" }, 
                    tests: { $each: "?array<?number>", $sort: -1 } , 
                    testsEmpty: { $each: "[]", $sort: 1 }
                } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ 
            cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
            command: "update", 
            q: { "HASH<x>": { "$eq": "?number" } }, 
            u: {
                $push: { 
                    "HASH<quizzes>": { $each: "?array<?object>", $sort: { "HASH<score>": 1, "HASH<name>": -1 } }, 
                    "HASH<scoresSinglePush>": { $each: "?array<?number>" }, 
                    "HASH<scoresToSlice>": { $each: "?array<?number>", $slice: "?number" }, 
                    "HASH<scoresWithPostion>": { $each: "?array<?number>", $position: "?number" }, 
                    "HASH<tests>": { $each: "?array<?number>", $sort: -1 } , 
                    "HASH<testsEmpty>": { $each: "[]", $sort: 1 }
                } 
            }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, PullModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ 
            { 
                q: { x: {$eq: 3} }, 
                u: { 
                    $pull: { instock: { $elemMatch: { qty: { $gt: 10, $lte: 20 } } } },
                    $pull: { pulledObjects: { testField: 6 } },
                    $pull: { arrayToPullFrom: 6 },
                    $pull: { results: { answers: { $elemMatch: { q: 2, a: { $gte: 8 } } } } },
                    $pull: { resultsWithoutPredicate: { q: 2, a: 8 } },
                    $pull: { "where.to.begin" : { "$regex" : "^thestart", "$options" : "" } },
                    $pull: {}
                },
                multi: false, 
                upsert: true 
            } ],
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
        cmdNs : {db : "testDB", coll : "testColl"},
        command : "update",
        q : {x : {$eq : 1}},
        u : {
            $pull : {
                arrayToPullFrom : 1,
                instock : {$elemMatch : {$and : [ {qty : {$gt : 1}}, {qty : {$lte : 1}} ]}},
                pulledObjects : {testField : {$eq : 1}},
                results : {answers : {$elemMatch : {$and : [ {q : {$eq : 1}}, {a : {$gte : 1}} ]}}},
                resultsWithoutPredicate : {$and : [ {q : {$eq : 1}}, {a : {$eq : 1}} ]},
                "where.to.begin": {"$regularExpression":{"pattern":"\\?","options":""}}
            }
        },
        multi : false,
        upsert : true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs : {db : "testDB", coll : "testColl"},
            command : "update",
            q : {x : {$eq : "?number"}},
            u : {
                $pull : {
                    arrayToPullFrom : "?number",
                    instock : {
                        $elemMatch :
                            {$and : [ {qty : {$gt : "?number"}}, {qty : {$lte : "?number"}} ]}
                    },
                    pulledObjects : {testField : {$eq : "?number"}},
                    results : {
                        answers : {
                            $elemMatch :
                                {$and : [ {q : {$eq : "?number"}}, {a : {$gte : "?number"}} ]}
                        }
                    },
                    resultsWithoutPredicate :
                        {$and : [ {q : {$eq : "?number"}}, {a : {$eq : "?number"}} ]},
                    "where.to.begin": {"$regularExpression":{"pattern":"?string","options":""}}
                }
            },
            multi : false,
            upsert : true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            cmdNs : {db : "HASH<testDB>", coll : "HASH<testColl>"},
            command : "update",
            q : {"HASH<x>" : {$eq : "?number"}},
            u : {
                $pull : {
                    "HASH<arrayToPullFrom>" : "?number",
                    "HASH<instock>" : {
                        $elemMatch :
                            {$and : [ {"HASH<qty>" : {$gt : "?number"}}, {"HASH<qty>" : {$lte : "?number"}} ]}
                    },
                    "HASH<pulledObjects>" : {"HASH<testField>" : {$eq : "?number"}},
                    "HASH<results>" : {
                        "HASH<answers>" : {
                            $elemMatch :
                                {$and : [ {"HASH<q>" : {$eq : "?number"}}, {"HASH<a>" : {$gte : "?number"}} ]}
                        }
                    },
                    "HASH<resultsWithoutPredicate>" :
                        {$and : [ {"HASH<q>" : {$eq : "?number"}}, {"HASH<a>" : {$eq : "?number"}} ]},
                    "HASH<where>.HASH<to>.HASH<begin>": {"$regularExpression":{"pattern":"?string","options":""}}
                }
            },
            multi : false,
            upsert : true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BitModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{  q: { x: {$eq: 3} },  u: { $bit: { expdata: { and: 10 } }, $bit: {} }, multi: false, upsert: true }],
        "$db": "testDB"})"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: { $bit: { expdata: { and: 1 } } },
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: {x: {$eq: "?number"}},  
            u: { $bit: { expdata: { and: "?number" } } },
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } },  
        u: { $bit: { "HASH<expdata>": { and: "?number" } } },
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, UnsetModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{ q: { x: {$eq: 3} },  u: {$unset : {tagsToRemove: {"this.should" : "beignored"}}, $unset: {}}, multi: false, upsert: true }],
        "$db": "testDB"})"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: {$unset : {tagsToRemove: 1}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: {x: {$eq: "?number"}},  
            u: {$unset : {tagsToRemove: 1}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } },  
        u: {$unset : {"HASH<tagsToRemove>": 1}}, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, RenameModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{ q: { x: {$eq: 3} },  u: {$rename : {oldName : "newName"}, $rename: {}}, multi: false, upsert: true }],
        "$db": "testDB"})"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: {$rename : {oldName : "newName"}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: {x: {$eq: "?number"}},  
            u: {$rename : {oldName : "newName"}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
            command: "update", 
            q: {"HASH<x>": {$eq: "?number"}},  
            u: {$rename : {"HASH<oldName>" : "HASH<newName>"}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, PopModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{ q: { x: {$eq: 3} },  u: {$pop: {popFirstElement : -1}, $pop : {popLastElement : 1}, $pop: {}}, multi: false, upsert: true }],
        "$db": "testDB"})"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: {$pop: {popFirstElement : -1, popLastElement : 1}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: {x: {$eq: "?number"}},  
            u: {$pop: {popFirstElement : -1, popLastElement : 1}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } },  
        u: {$pop: {"HASH<popFirstElement>": -1, "HASH<popLastElement>": 1}}, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, PullAllModifierUpdateShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [{ q: { x: {$eq: 3} },  u: {$pullAll: {colorsToRemove: [ "red", "blue" ]}, $pullAll: {colorsToRemoveEmpty: []}, $pullAll: {}}, multi: false, upsert: true }],
        "$db": "testDB"})"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: { x: { $eq: 1 } }, 
            u: { $pullAll: { colorsToRemove: [ "?" ], colorsToRemoveEmpty: [] } }, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "testDB", coll: "testColl" }, 
            command: "update", 
            q: {x: {$eq: "?number"}},  
            u: {$pullAll: {colorsToRemove: "?array<?string>", "colorsToRemoveEmpty": "[]"}}, 
            multi: false, 
            upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({ cmdNs: { db: "HASH<testDB>", coll: "HASH<testColl>" }, 
        command: "update", 
        q: { "HASH<x>": { $eq: "?number" } },  
        u: {$pullAll: { "HASH<colorsToRemove>": "?array<?string>", "HASH<colorsToRemoveEmpty>": "[]" }}, 
        multi: false, 
        upsert: true })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugShapeAndMarkIdentifiers_FOR_TEST,
                     SerializationContext::stateDefault()));
}

/**
 * Test that expressions in a pipeline-style update are properly shapified.
 * As a side effect, this also tests that we don't optimize the pipeline during shapification.
 * Expressions should be preserved in their original form, not pre-computed.
 */
TEST_F(UpdateCmdShapeTest, PipelineUpdateWithExpressionsShape) {
    // Expressions like {$add: [1, 2]} remain as expressions and are not folded to constant values
    // like 3.
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { x: 1 }, 
            u: [ 
                { 
                    "$set": { 
                        "computed": { "$add": [1, 2] },
                        "product": { "$multiply": [3, 4] },
                        "nested": { "$add": [{ "$multiply": [2, 3] }, 1] }
                    } 
                } 
            ],
            multi: false, 
            upsert: false 
        } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": [
                {
                    "$set": {
                        "computed": {
                            "$add": "?array<?number>"
                        },
                        "product": {
                            "$multiply": "?array<?number>"
                        },
                        "nested": {
                            "$add": [
                                {
                                    "$multiply": "?array<?number>"
                                },
                                "?number"
                            ]
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": [
                {
                    "$set": {
                        "computed": {
                            "$add": [1, 1]
                        },
                        "product": {
                            "$multiply": [1, 1]
                        },
                        "nested": {
                            "$add": [
                                {
                                    "$multiply": [1, 1]
                                },
                                1
                            ]
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, PipelineUpdateWithConstantsShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { x: 1 }, 
            u: [ { "$set": { "foo": "$$myVar", "num": "$$myNum" } } ],
            c: { "myVar": "hello", "myNum": 42 },
            multi: false, 
            upsert: false 
        } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": [
                {
                    "$set": {
                        "foo": "$$myVar",
                        "num": "$$myNum"
                    }
                }
            ],
            "c": {
                "myVar": "?string",
                "myNum": "?number"
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": [
                {
                    "$set": {
                        "foo": "$$myVar",
                        "num": "$$myNum"
                    }
                }
            ],
            "c": {
                "myVar": "?",
                "myNum": 1
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

/**
 * Test that complicated things like object literals are properly shapified when they are in the "c"
 * field.
 */
TEST_F(UpdateCmdShapeTest, PipelineUpdateWithComplexConstantsShape) {
    // Even though "nestedObject" has expression-like syntax, it should be treated as an object
    // literal.
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { x: 1 }, 
            u: [ { "$set": { "x": "$$simpleObject", "nested": "$$nestedObject" } } ],
            c: { 
                "simpleObject": { "a": 1 },
                "nestedObject": { "$pretendField": [{ "$anotherFakeOne": [3, 4] }, 5] }
            },
            multi: false, 
            upsert: false 
        } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": [
                {
                    "$set": {
                        "x": "$$simpleObject",
                        "nested": "$$nestedObject"
                    }
                }
            ],
            "c": {
                "simpleObject": "?object",
                "nestedObject": "?object"
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": [
                {
                    "$set": {
                        "x": "$$simpleObject",
                        "nested": "$$nestedObject"
                    }
                }
            ],
            "c": {
                "simpleObject": {
                    "?": "?"
                },
                "nestedObject": {
                    "?": "?"
                }
            },
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

/**
 * Test that a pipeline update with all allowed stages ($addFields, $project, $replaceRoot) is
 * properly shapified.
 */
TEST_F(UpdateCmdShapeTest, PipelineUpdateWithAllAllowedStagesShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { x: {$eq: 3} }, 
            u: [ 
                { "$addFields": { "newField": "value", "count": 42 } },
                { "$project": { "oldField": 0, "_id": 1 } }, 
                { "$replaceRoot": { "newRoot": { "finalDoc": "$$ROOT", "processed": true } } }
            ], 
            multi: false, 
            upsert: false 
        } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": [
                {
                    "$addFields": {
                        "newField": "?string",
                        "count": "?number"
                    }
                },
                {
                    "$project": {
                        "oldField": false,
                        "_id": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "finalDoc": "$$ROOT",
                            "processed": "?bool"
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": [
                {
                    "$addFields": {
                        "newField": {
                            "$const": "?"
                        },
                        "count": {
                            "$const": 1
                        }
                    }
                },
                {
                    "$project": {
                        "oldField": false,
                        "_id": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "finalDoc": "$$ROOT",
                            "processed": {
                                "$const": true
                            }
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": 1
                }
            },
            "u": [
                {
                    "$addFields": {
                        "newField": {
                            "$const": "?"
                        },
                        "count": {
                            "$const": 1
                        }
                    }
                },
                {
                    "$project": {
                        "oldField": false,
                        "_id": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "finalDoc": "$$ROOT",
                            "processed": {
                                "$const": true
                            }
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": false
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

/**
 * Test that stage aliases get shapified properly.
 * - $set is an alias for $addFields and will be preserved.
 * - $unset will desguar to $project
 * - $replaceWith will desugar to $replaceRoot
 */
TEST_F(UpdateCmdShapeTest, PipelineUpdateWithStageAliasesShape) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { y: {$gt: 5} }, 
            u: [ 
                { "$set": { "status": "updated", "version": 2 } },
                { "$unset": "tempField" }, 
                { "$replaceWith": { "newDoc": "$$ROOT", "timestamp": "$$NOW" } } 
            ], 
            multi: true, 
            upsert: true 
        } ],
        "$db": "testDB"
    })"_sd);

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "y": {
                    "$gt": "?number"
                }
            },
            "u": [
                {
                    "$set": {
                        "status": "?string",
                        "version": "?number"
                    }
                },
                {
                    "$project": {
                        "tempField": false,
                        "_id": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "newDoc": "$$ROOT",
                            "timestamp": "$$NOW"
                        }
                    }
                }
            ],
            "multi": true,
            "upsert": true
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "y": {
                    "$gt": 1
                }
            },
            "u": [
                {
                    "$set": {
                        "status": {
                            "$const": "?"
                        },
                        "version": {
                            "$const": 1
                        }
                    }
                },
                {
                    "$project": {
                        "tempField": false,
                        "_id": true
                    }
                },
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "newDoc": "$$ROOT",
                            "timestamp": "$$NOW"
                        }
                    }
                }
            ],
            "multi": true,
            "upsert": true
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BatchReplacementUpdateShape) {
    auto shapes = makeShapesFromUpdate(R"({
        update: "testColl",
        updates: [
          { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false },
          { q: { x: {$gt: 3}, y: "foo" }, u: { x: {y: 100}, z: false }, multi: false, upsert: true }
        ],
        "$db": "testDB"
    })"_sd);
    ASSERT_EQ(shapes.size(), 2);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": false
        })",
        shapes[0].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "$and": [
                    {
                        "x": {
                            "$gt": "?number"
                        }
                    },
                    {
                        "y": {
                            "$eq": "?string"
                        }
                    }
                ]
            },
            "u": "?object",
            "multi": false,
            "upsert": true
        })",
        shapes[1].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BatchModifierUpdateShape) {
    auto shapes = makeShapesFromUpdate(R"({
        update: "testColl",
        updates: [
          { q: { x: {$eq: 3} }, u: { $unset: { foo: 1 }}, multi: false, upsert: false },
          { q: { x: {$gt: 3}, y: "foo" }, u: {$set: { x: {y: 100}, z: false }}, multi: true, upsert: true }
        ],
        "$db": "testDB"
    })"_sd);
    ASSERT_EQ(shapes.size(), 2);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": {$unset: { foo: 1 }},
            "multi": false,
            "upsert": false
        })",
        shapes[0].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "$and": [
                    {
                        "x": {
                            "$gt": "?number"
                        }
                    },
                    {
                        "y": {
                            "$eq": "?string"
                        }
                    }
                ]
            },
            "u": {$set: { x: "?object", z: "?bool" }},
            "multi": true,
            "upsert": true
        })",
        shapes[1].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, BatchPipelineUpdateShape) {
    auto shapes = makeShapesFromUpdate(R"({
        update: "testColl",
        updates: [
          { q: { y: { "$gt": 5 } }, u: [ { "$unset": "oldField" }, { "$set": { "status": "updated" } } ], multi: true, upsert: false },
          { q: { z: true }, u: [ { "$replaceWith": { "newDoc": "$$ROOT", "timestamp": "$$NOW" } } ], multi: false, upsert: true }
        ],
        "$db": "testDB"
    })"_sd);
    ASSERT_EQ(shapes.size(), 2);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "y": {
                    "$gt": "?number"
                }
            },
            "u": [
                {
                    "$project": {
                        "oldField": false,
                        "_id": true
                    }
                },
                {
                    "$set": {
                        "status": "?string"
                    }
                }
            ],
            "multi": true,
            "upsert": false
        })",
        shapes[0].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "z": {
                    "$eq": "?bool"
                }
            },
            "u": [
                {
                    "$replaceRoot": {
                        "newRoot": {
                            "newDoc": "$$ROOT",
                            "timestamp": "$$NOW"
                        }
                    }
                }
            ],
            "multi": false,
            "upsert": true
        })",
        shapes[1].toBson(_operationContext.get(),
                         SerializationOptions::kDebugQueryShapeSerializeOptions,
                         SerializationContext::stateDefault()));
}

TEST_F(UpdateCmdShapeTest, IncludesOptionalValues) {
    // Test setting optional values such as 'multi' and 'upsert' to verify that they are included in
    // the query shape.
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: true } ],
        ordered: false,
        bypassDocumentValidation: true,
        let: {x: 4, y: "abc"},
        "$db": "testDB"
    })"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "update",
            "q": {
                "x": {
                    "$eq": "?number"
                }
            },
            "u": "?object",
            "multi": false,
            "upsert": true,
            "let": {
                "x": "?number",
                "y": "?string"
            }
        })",
        shape.toBson(_operationContext.get(),
                     SerializationOptions::kDebugQueryShapeSerializeOptions,
                     SerializationContext::stateDefault()));
}

// Verifies that "update" command shape hash value is stable (does not change between the
// versions of the server).
TEST_F(UpdateCmdShapeTest, StableQueryShapeHashValue) {
    UpdateCmdBuilder updateCmd;
    updateCmd.database = "testDB";
    updateCmd.collection = "testColl";
    updateCmd.q = BSON("x" << BSON("$eq" << 3));
    updateCmd.u = BSON("foo" << "bar");
    auto serializationContext = SerializationContext::stateCommandRequest();

    auto verifyHash = [&](StringData expectedHash, const UpdateCmdBuilder& updateCmd) {
        BSONObj updateBson = updateCmd.toBSON();
        auto updateRequest = UpdateCommandRequest::parseOwned(std::move(updateBson));
        auto shapes = makeShapesFromUpdateRequest(updateRequest);
        ASSERT_EQ(shapes.size(), 1);
        const auto& shape = shapes.front();

        const auto hash = shape.sha256Hash(_operationContext.get(), serializationContext);
        ASSERT_EQ(expectedHash, hash.toHexString());
    };

    std::string expectedHash = "9BE397CDA2A946681D8E330532C9F04FD4B06C2D32CA75CDA47D59E771043D36";
    verifyHash(expectedHash, updateCmd);

    // Changing the literal value in the query should not change the hash.
    updateCmd.q = BSON("x" << BSON("$eq" << 4));
    verifyHash(expectedHash, updateCmd);

    // Changing the field name in the query should change the hash.
    updateCmd.q = BSON("y" << BSON("$eq" << 4));
    expectedHash = "4FD91619E9D50806A419D658D2978A2B3E931E0AA709D785F2D24028A2CF4DFC";
    verifyHash(expectedHash, updateCmd);

    // Setting upsert to false explicitly should not change the hash.
    updateCmd.upsert = false;
    verifyHash(expectedHash, updateCmd);

    // Setting upsert from false to true should change the hash.
    updateCmd.upsert = true;
    expectedHash = "F6C4A4A410028676E3E51E4E1A8BE8169C8C6281AF929F029E3E437D3855D809";
    verifyHash(expectedHash, updateCmd);

    // Setting a let should change the hash.
    updateCmd.let = BSON("z" << "abc");
    expectedHash = "E153D6789FAF6EBEC990308CA2C29EABE03BCB5C9BE3CDF7C2E777A3DC55A683";
    verifyHash(expectedHash, updateCmd);

    // Setting a collation should change the hash.
    updateCmd.collation = BSON("locale" << "fr");
    expectedHash = "B5BFED37B52DF311A4369CFA6905329F02578207E6C14696AC7E1C9F1CC43990";
    verifyHash(expectedHash, updateCmd);

    // Changing the collection should change the hash.
    updateCmd.collection = "testColl2";
    expectedHash = "DF3AD468A336C41ABE3992A72F24DEC28E349B4D27614D19E4C2C9E8F3655F2E";
    verifyHash(expectedHash, updateCmd);

    // Changing the update to a pipeline style should change the hash.
    updateCmd.u = BSON_ARRAY(BSON("$set" << BSON("foo" << "hello" << "num" << 42)));
    expectedHash = "A6DDEE3675054450EA9BA40B25F7FF02897970502E084307A2539B9E18C8DB14";
    verifyHash(expectedHash, updateCmd);

    // Adding constants should change the hash.
    updateCmd.c = BSON("myVar" << "hello" << "myNum" << 42);
    expectedHash = "48BC89506D10C460EDEEFA55233CAFF58F07E21E1F6CEEFA78FB97C9EEC300E2";
    verifyHash(expectedHash, updateCmd);

    // Changing the literal values to constants should change the hash.
    updateCmd.u = BSON_ARRAY(fromjson(R"({ "$set": { "foo": "$$myVar", "num": "$$myNum" }})"_sd));
    expectedHash = "474D7DED0A5B2CB2FE8872E8144050C4AAC3A2C6E35A6B48ADB61DA277EF64A0";
    verifyHash(expectedHash, updateCmd);

    // Setting multi to false explicitly should not change the hash.
    updateCmd.multi = false;
    verifyHash(expectedHash, updateCmd);

    // Setting multi from false to true should change the hash.
    updateCmd.multi = true;
    expectedHash = "F373308007DC7A9F0BC2A7D2BD1862E77EEE1D1EDE45C9618C20905A674A2815";
    verifyHash(expectedHash, updateCmd);

    // Changing update to a modifier style should change the hash.
    updateCmd.c = boost::none;
    updateCmd.u = fromjson(R"({ "$set": { "foo": "bar", "num": "mynum" }})"_sd);
    expectedHash = "670D08BB8ECAC87CDEF7AB4B600E08F7F1418767858A4FAB23A7478DFDFEF83C";
    verifyHash(expectedHash, updateCmd);

    // TODO(SERVER-113907): When 'representativeArrayFilters' is supported, test hash stability when
    // 'representativeArrayFilters' is added.
}

TEST_F(UpdateCmdShapeTest, SizeOfUpdateCmdShapeComponents) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    validateShapeSize(shape);
}

TEST_F(UpdateCmdShapeTest, SizeOfUpdateCmdShapeComponentsForModifierUpdate) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: {$set: {foo: "bar" }}, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);

    // TODO SERVER-113907: When 'representativeArrayFilters' is supported, add tests for validating
    // shape size with arrayFilters.
    validateShapeSize(shape);
}

TEST_F(UpdateCmdShapeTest, SizeOfUpdateCmdShapeComponentsWithPipelineAndConstants) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { 
            q: { x: {$eq: 3} }, 
            u: [ { "$set": { "foo": "$$myVar", "num": "$$myNum" } } ],
            c: { "myVar": "hello", "myNum": 42 },
            multi: false, 
            upsert: false 
        } ],
        "$db": "testDB"
    })"_sd);

    auto updateComponents =
        static_cast<const UpdateCmdShapeComponents&>(shape.specificComponents());
    ASSERT(updateComponents.representativeC);
    validateShapeSize(shape);
}

TEST_F(UpdateCmdShapeTest, EquivalentUpdateCmdShapeSizes) {
    auto shape = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: false } ],
        "$db": "testDB"
    })"_sd);
    auto shapeOptionalValues = makeOneShapeFromUpdate(R"({
        update: "testColl",
        updates: [ { q: { x: {$eq: 3} }, u: { foo: "bar" }, multi: false, upsert: true } ],
        "$db": "testDB",
        ordered: false,
        bypassDocumentValidation: true
    })"_sd);
    ASSERT_EQ(shape.size(), shapeOptionalValues.size());
}
}  // namespace
}  // namespace mongo::query_shape
