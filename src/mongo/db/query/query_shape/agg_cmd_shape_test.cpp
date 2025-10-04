/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_shape/agg_cmd_shape.h"

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/let_shape_component.h"
#include "mongo/db/query/query_shape/shape_helpers.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_shape {

namespace {
static const NamespaceString kDefaultTestNss =
    NamespaceString::createNamespaceString_forTest("testDB.testColl");

class AggCmdShapeTest : public unittest::Test {
public:
    void setUp() final {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _expCtx = make_intrusive<ExpressionContextForTest>();
    }

    std::unique_ptr<AggregateCommandRequest> makeAggregateCommandRequest(
        std::vector<StringData> stagesJson,
        boost::optional<StringData> letJson = boost::none,
        boost::optional<StringData> collationJson = boost::none) {
        std::vector<BSONObj> pipeline;
        for (auto&& stage : stagesJson) {
            pipeline.push_back(fromjson(stage));
        }

        auto aggRequest =
            std::make_unique<AggregateCommandRequest>(kDefaultTestNss, std::move(pipeline));
        if (letJson) {
            aggRequest->setLet(fromjson(*letJson));
        }
        if (collationJson) {
            aggRequest->setCollation(fromjson(*collationJson));
        }
        return aggRequest;
    }

    std::unique_ptr<AggCmdShape> makeShapeFromPipeline(
        std::vector<StringData> stagesJson,
        boost::optional<StringData> letJson = boost::none,
        boost::optional<StringData> collationJson = boost::none) {

        auto aggRequest = makeAggregateCommandRequest(
            std::move(stagesJson), std::move(letJson), std::move(collationJson));

        auto parsedPipeline = Pipeline::parse(aggRequest->getPipeline(), _expCtx);
        return std::make_unique<AggCmdShape>(*aggRequest,
                                             kDefaultTestNss,
                                             stdx::unordered_set<NamespaceString>{kDefaultTestNss},
                                             *parsedPipeline,
                                             _expCtx);
    }

    std::unique_ptr<AggCmdShapeComponents> makeShapeComponentsFromPipeline(
        std::vector<StringData> stagesJson, OptionalBool allowDiskUse = {}) {
        auto aggRequest = makeAggregateCommandRequest(std::move(stagesJson));

        auto parsedPipeline = Pipeline::parse(aggRequest->getPipeline(), _expCtx);
        return std::make_unique<AggCmdShapeComponents>(
            *aggRequest,
            stdx::unordered_set<NamespaceString>{kDefaultTestNss},
            parsedPipeline->serializeToBson(
                SerializationOptions::kRepresentativeQueryShapeSerializeOptions),
            LetShapeComponent(aggRequest->getLet(), _expCtx));
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

TEST_F(AggCmdShapeTest, BasicPipelineShape) {
    auto shape =
        makeShapeFromPipeline({R"({$match: {x: 3, y: {$lte: 3}}})"_sd,
                               R"({$group: {_id: "$y", z: {$max: "$z"}, w: {$avg: "$w"}}})"});
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "command": "aggregate",
            "pipeline": [
                {
                    "$match": {
                        "$and": [
                            {
                                "x": {
                                    "$eq": "?number"
                                }
                            },
                            {
                                "y": {
                                    "$lte": "?number"
                                }
                            }
                        ]
                    }
                },
                {
                    "$group": {
                        "_id": "$y",
                        "z": {
                            "$max": "$z"
                        },
                        "w": {
                            "$avg": "$w"
                        }
                    }
                }
            ]
        })",
        shape->toBson(_operationContext.get(),
                      SerializationOptions::kDebugQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

TEST_F(AggCmdShapeTest, IncludesLet) {
    auto shape = makeShapeFromPipeline({R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd},
                                       R"({x: 4, y: "str"})"_sd);
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "let": {
                "x": "?number",
                "y": "?string"
            },
            "command": "aggregate",
            "pipeline": [
                {
                    "$match": {
                        "x": {
                            "$eq": "?number"
                        }
                    }
                },
                {
                    "$limit": "?number"
                }
            ]
        })",
        shape->toBson(_operationContext.get(),
                      SerializationOptions::kDebugQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "cmdNs": {
                "db": "testDB",
                "coll": "testColl"
            },
            "let": {
                "x": {
                    "$const": 1
                },
                "y": {
                    "$const": "?"
                }
            },
            "command": "aggregate",
            "pipeline": [
                {
                    "$match": {
                        "x": {
                            "$eq": 1
                        }
                    }
                },
                {
                    "$limit": 1
                }
            ]
        })",
        shape->toBson(_operationContext.get(),
                      SerializationOptions::kRepresentativeQueryShapeSerializeOptions,
                      SerializationContext::stateDefault()));
}

// Verifies that "aggregate" command shape hash value is stable (does not change between the
// versions of the server).
TEST_F(AggCmdShapeTest, StableQueryShapeHashValue) {
    auto shape = makeShapeFromPipeline({R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd},
                                       R"({x: 4, y: "str"})"_sd);

    auto serializationContext = SerializationContext::stateCommandRequest();
    const auto hash = shape->sha256Hash(_operationContext.get(), serializationContext);
    ASSERT_EQ("F5C9B81418515DE73EA3F500F7BBB7AD5ED60D4DD9E5F645825730BC0E633BF2",
              hash.toHexString());
}

TEST_F(AggCmdShapeTest, SizeOfAggCmdShapeComponents) {
    auto aggComponents = makeShapeComponentsFromPipeline(
        {R"({$match: {x: 3, y: {$lte: 3}}})"_sd,
         R"({$group: {_id: "$y", z: {$max: "$z"}, w: {$avg: "$w"}}})"},
        false /*allowDiskUse*/);

    // The sizes of any members of AggCmdShapeComponents are typically accounted for by
    // sizeof(AggCmdShapeComponents). The important part of the test here is to ensure that any
    // additional memory allocations are also included in the size() operation. In our case,
    // we expect additional memory use from the representative pipeline and the involved
    // namespaces set.
    const auto pipelineSize = shape_helpers::containerSize(aggComponents->representativePipeline);
    const auto involvedNamespacesSize = sizeof(kDefaultTestNss) +
        kDefaultTestNss.size();  // kDefaultTestNss is the only value in the unordered set.
    const auto letSize = aggComponents->let.size();

    ASSERT_EQ(aggComponents->size(),
              sizeof(AggCmdShapeComponents) + pipelineSize + involvedNamespacesSize + letSize -
                  sizeof(LetShapeComponent));
}

TEST_F(AggCmdShapeTest, EquivalentAggCmdShapeComponentSizes) {
    auto aggComponentsDiskUseFalse = makeShapeComponentsFromPipeline(
        {R"({$match: {x: 3, y: {$lte: 3}}})"_sd,
         R"({$group: {_id: "$y", z: {$max: "$z"}, w: {$avg: "$w"}}})"},
        false /*allowDiskUse*/);
    auto aggComponentsDiskUseTrue = makeShapeComponentsFromPipeline(
        {R"({$match: {x: 3, y: {$lte: 3}}})"_sd,
         R"({$group: {_id: "$y", z: {$max: "$z"}, w: {$avg: "$w"}}})"},
        true /*allowDiskUse*/);
    ASSERT_EQ(aggComponentsDiskUseFalse->size(), aggComponentsDiskUseTrue->size());
}

TEST_F(AggCmdShapeTest, DifferentAggCmdShapeComponentSizes) {
    auto smallAggComponents = makeShapeComponentsFromPipeline({R"({$match: {x: 3, y: {$lte: 3}}})"},
                                                              false /*allowDiskUse*/);
    auto largeAggComponents = makeShapeComponentsFromPipeline(
        {R"({$match: {x: 3, y: {$lte: 3}}})"_sd,
         R"({$group: {_id: "$y", z: {$max: "$z"}, w: {$avg: "$w"}}})"},
        false /*allowDiskUse*/);
    ASSERT_LT(smallAggComponents->size(), largeAggComponents->size());
}

TEST_F(AggCmdShapeTest, SizeOfAggCmdShapeWithAndWithoutLet) {
    auto shapeWithoutLet = makeShapeFromPipeline({R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd});
    auto shapeWithLet = makeShapeFromPipeline({R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd},
                                              R"({x: 4, y: "str"})"_sd);
    ASSERT_LT(shapeWithoutLet->size(), shapeWithLet->size());
}

TEST_F(AggCmdShapeTest, SizeOfAggCmdShapeWithAndWithoutCollation) {
    auto shapeWithoutCollation =
        makeShapeFromPipeline({R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd});
    auto shapeWithCollation = makeShapeFromPipeline(
        {R"({$match: {x: 3}})"_sd, R"({$limit: 2})"_sd}, boost::none, R"({locale: "en_US"})"_sd);
    ASSERT_LT(shapeWithoutCollation->size(), shapeWithCollation->size());
}
}  // namespace
}  // namespace mongo::query_shape
