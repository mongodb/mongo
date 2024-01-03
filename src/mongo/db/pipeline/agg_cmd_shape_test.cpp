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

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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

    auto makeShapeFromPipeline(std::vector<StringData> stagesJson,
                               boost::optional<StringData> letJson = boost::none) {
        std::vector<BSONObj> pipeline;
        for (auto&& stage : stagesJson) {
            pipeline.push_back(fromjson(stage));
        }
        AggregateCommandRequest aggRequest(kDefaultTestNss, pipeline);
        if (letJson)
            aggRequest.setLet(fromjson(*letJson));
        auto parsedPipeline = Pipeline::parse(std::move(pipeline), _expCtx);

        return std::make_unique<AggCmdShape>(aggRequest,
                                             kDefaultTestNss,
                                             stdx::unordered_set<NamespaceString>{kDefaultTestNss},
                                             *parsedPipeline,
                                             _expCtx);
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
                "x":  1,
                "y": "?"
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
}  // namespace

}  // namespace mongo::query_shape
