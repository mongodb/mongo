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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_comparator.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/document_source_set_window_fields.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

// This provides access to getExpCtx(), but we'll use a different name for this test suite.
using DocumentSourceSetWindowFieldsTest = AggregationContextFixture;

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseInvalidArgumentTypes) {
    auto spec = BSON("$_internalSetWindowFields"
                     << "invalid");
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);

    spec = BSON("$_internalSetWindowFields" << BSON("sortBy"
                                                    << "invalid sort spec"));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);

    spec = BSON("$_internalSetWindowFields" << BSON("output"
                                                    << "invalid"));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);

    spec = BSON("$_internalSetWindowFields"
                << BSON("partitionBy" << BSON("$notAnExpression" << 1) << "output" << BSONObj()));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::InvalidPipelineOperator);

    spec = BSON("$_internalSetWindowFields" << BSON("unknown_parameter" << 1));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        40415);
}

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseIfArgumentsAreRepeated) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', $max: '$pop', window: {documents: [-10, 0]}}}}})");
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseIfWindowFieldHasExtraArgument) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: [0, 10], document: [0,8]} }}}})");
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);
}

TEST_F(DocumentSourceSetWindowFieldsTest, SuccessfullyParsesAndReserializes) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: [-10, 0]}}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    std::vector<Value> serializedArray;
    parsedStage->serializeToArray(serializedArray);
    ASSERT_BSONOBJ_EQ(serializedArray[0].getDocument().toBson(), spec);
}

TEST_F(DocumentSourceSetWindowFieldsTest, SuccessfullyParsesOnceFeatureFlagEnabled) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: [-10, 0]}}}}})");
    // By default, the unit test will have the feature flag enabled.
    auto pipeline = Pipeline::parse(std::vector<BSONObj>({spec}), getExpCtx());
    ASSERT_BSONOBJ_EQ(pipeline->serializeToBson()[0], spec);
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesEmptyInputCorrectly) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: ["unbounded", 0]}}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    const auto mock = DocumentSourceMock::createForTest(getExpCtx());
    parsedStage->setSource(mock.get());
    ASSERT_EQ((int)DocumentSource::GetNextResult::ReturnStatus::kEOF,
              (int)parsedStage->getNext().getStatus());
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesDependencyWithArrayExpression) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$a', sortBy: {b: 1}, output: {myCov:
        {$covariancePop: ['$c', '$d']}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_EQUALS(parsedStage->getDependencies(&deps), DepsTracker::State::SEE_NEXT);
    ASSERT_EQUALS(deps.fields.size(), 4U);
    ASSERT_EQUALS(deps.fields.count("a"), 1U);
    ASSERT_EQUALS(deps.fields.count("b"), 1U);
    ASSERT_EQUALS(deps.fields.count("c"), 1U);
    ASSERT_EQUALS(deps.fields.count("d"), 1U);
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesDependencyWithNoSort) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$a', output: {myAvg:
        {$avg: '$c'}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_EQUALS(parsedStage->getDependencies(&deps), DepsTracker::State::SEE_NEXT);
    ASSERT_EQUALS(deps.fields.size(), 2U);
    ASSERT_EQUALS(deps.fields.count("a"), 1U);
    ASSERT_EQUALS(deps.fields.count("c"), 1U);
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesDependencyWithNoPartitionBy) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {output: {myAvg:
        {$avg: '$c'}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_EQUALS(parsedStage->getDependencies(&deps), DepsTracker::State::SEE_NEXT);
    ASSERT_EQUALS(deps.fields.size(), 1U);
    ASSERT_EQUALS(deps.fields.count("c"), 1U);
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesDependencyWithNoInputDependency) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {output: {myCount:
        {$sum: 1}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_EQUALS(parsedStage->getDependencies(&deps), DepsTracker::State::SEE_NEXT);
    ASSERT_EQUALS(deps.fields.size(), 0U);
}

TEST_F(DocumentSourceSetWindowFieldsTest, HandlesImplicitDependencyForDottedOutputField) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {output: {'x.y.z':
        {$sum: 1}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    DepsTracker deps(DepsTracker::kAllMetadata);
    ASSERT_EQUALS(parsedStage->getDependencies(&deps), DepsTracker::State::SEE_NEXT);
    ASSERT_EQUALS(deps.fields.size(), 2U);
    ASSERT_EQUALS(deps.fields.count("x"), 1U);
    ASSERT_EQUALS(deps.fields.count("x.y"), 1U);
    ASSERT_EQUALS(deps.fields.count("x.y.z"), 0U);
}

TEST_F(DocumentSourceSetWindowFieldsTest, ReportsModifiedFields) {
    auto spec = fromjson(R"(
        {$_internalSetWindowFields: {output: {a:
        {$sum: 1}, b: {$sum: 1}}}})");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    auto modified = parsedStage->getModifiedPaths();
    ASSERT_TRUE(modified.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQUALS(modified.paths.size(), 2U);
    ASSERT_EQUALS(modified.paths.count("a"), 1U);
    ASSERT_EQUALS(modified.paths.count("b"), 1U);
    ASSERT_TRUE(modified.renames.empty());
}

TEST_F(DocumentSourceSetWindowFieldsTest, RedactionOnShiftOperator) {
    auto spec = fromjson(
        R"({
            $setWindowFields: {
                partitionBy: '$foo',
                sortBy: {
                    bar: 1
                },
                output: {
                    x: {
                        $shift: {
                            output: '$y',
                            by: 1,
                            default: 'BAZ'
                        }
                    }
                }
            }
        })");
    auto docSource =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSetWindowFields": {
                "partitionBy": "$HASH<foo>",
                "sortBy": {
                    "HASH<bar>": 1
                },
                "output": {
                    "HASH<x>": {
                        "$shift": {
                            by: "?",
                            output: "$HASH<y>",
                            default: "?"
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RedactionOnExpMovingAvgOperator) {
    auto spec = fromjson(
        R"({
            $setWindowFields: {
                partitionBy: '$foo.bar',
                sortBy: {
                    bar: 1
                },
                output: {
                    x: {
                        $expMovingAvg: {
                            alpha: 0.5,
                            input: '$y'
                        }
                    }
                }
            }
        })");
    auto docSource =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSetWindowFields": {
                "partitionBy": "$HASH<foo>.HASH<bar>",
                "sortBy": {
                    "HASH<bar>": 1
                },
                "output": {
                    "HASH<x>": {
                        "$expMovingAvg": {
                            alpha: "?",
                            input: "$HASH<y>"
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RedactionOnExpressionNOperator) {
    auto spec = fromjson(
        R"({
            $setWindowFields: {
                partitionBy: '$a',
                output: {
                    b: {
                        $minN: {
                            n: 3,
                            input: '$y'
                        }
                    }
                }
            }
        })");
    auto docSource =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSetWindowFields": {
                "partitionBy": "$HASH<a>",
                "output": {
                    "HASH<b>": {
                        "$minN": {
                            "n": {
                                "$const": "?"
                            },
                            "input": "$HASH<y>"
                        },
                        "window": {
                            "documents": [
                                "unbounded",
                                "unbounded"
                            ]
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RedactionOnSumWithDocumentsWindow) {
    auto spec = fromjson(R"({
        $setWindowFields: {
            partitionBy: {
                $year: '$x'
            },
            sortBy: {
                a: 1,
                b: -1
            },
            output: {
                cumulative: {
                    $sum: '$baz',
                    window: {
                        documents: [
                            'unbounded',
                            'current'
                        ]
                    }
                },
                maximum: {
                    $max: '$baz',
                    window: {
                        documents: [
                            'unbounded',
                            'unbounded'
                        ]
                    }
                }
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSetWindowFields": {
                "partitionBy": {
                    "$year": {
                        "date": "$HASH<x>"
                    }
                },
                "sortBy": {
                    "HASH<a>": 1,
                    "HASH<b>": -1
                },
                "output": {
                    "HASH<cumulative>": {
                        "$sum": "$HASH<baz>",
                        "window": {
                            "documents": [
                                "unbounded",
                                "current"
                            ]
                        }
                    },
                    "HASH<maximum>": {
                        "$max": "$HASH<baz>",
                        "window": {
                            "documents": [
                                "unbounded",
                                "unbounded"
                            ]
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RedactionOnPushWithRangeWindowWithUnit) {
    auto spec = fromjson(R"({
        $setWindowFields: {
            partitionBy: '$foo',
            sortBy: {
                bar: 1
            },
            output: {
                a: {
                    $push: '$b',
                    window: {
                        range: [
                            'unbounded',
                            -10
                        ],
                        unit: 'month'
                    }
                }
            }
        }
    })");
    auto docSource =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({
            "$_internalSetWindowFields": {
                "partitionBy": "$HASH<foo>",
                "sortBy": {
                    "HASH<bar>": 1
                },
                "output": {
                    "HASH<a>": {
                        "$push": "$HASH<b>",
                        "window": {
                            "range": [
                                "unbounded",
                                "?"
                            ],
                            "unit": "?"
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}
}  // namespace
}  // namespace mongo
