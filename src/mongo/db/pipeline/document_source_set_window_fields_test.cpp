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

#include "mongo/db/pipeline/document_source_set_window_fields.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/mock_stage.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/process_interface/standalone_process_interface.h"
#include "mongo/db/query/plan_summary_stats_visitor.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <vector>

namespace mongo {
namespace {

/**
 * Fixture that provides expression context.
 */
using DocumentSourceSetWindowFieldsTest = AggregationContextFixture;

/**
 * Fixture that also provides storage engine for spilling.
 */
class DocumentSourceSetWindowFieldsSpillingTest : public AggregationContextFixture {
public:
    DocumentSourceSetWindowFieldsSpillingTest()
        : AggregationContextFixture(std::make_unique<MongoDScopedGlobalServiceContextForTest>(
              MongoDScopedGlobalServiceContextForTest::Options{}, shouldSetupTL)) {
        getExpCtx()->setMongoProcessInterface(
            std::make_shared<StandaloneProcessInterface>(nullptr));
    }
};

TEST_F(DocumentSourceSetWindowFieldsTest, FailsToParseInvalidArgumentTypes) {
    auto spec = BSON("$_internalSetWindowFields" << "invalid");
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::FailedToParse);

    spec = BSON("$_internalSetWindowFields" << BSON("sortBy" << "invalid sort spec"));
    ASSERT_THROWS_CODE(
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx()),
        AssertionException,
        ErrorCodes::TypeMismatch);

    spec = BSON("$_internalSetWindowFields" << BSON("output" << "invalid"));
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
        ErrorCodes::IDLUnknownField);
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
    auto stage = exec::agg::buildStage(parsedStage);
    const auto mock = exec::agg::MockStage::createForTest({}, getExpCtx());
    stage->setSource(mock.get());
    ASSERT_EQ((int)DocumentSource::GetNextResult::ReturnStatus::kEOF,
              (int)stage->getNext().getStatus());
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
                            "by": "?number",
                            "output": "$HASH<y>",
                            "default": "?string"
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

TEST_F(DocumentSourceSetWindowFieldsTest, PartitionOutputIsCorrect) {
    auto spec = fromjson(  // NOLINT
        R"({
            "$setWindowFields": {
                "sortBy": { "num": 1 },
                "output": {
                    "sum": {
                        "$sum": "$val",
                        window: {
                            documents: [ -1 , 1 ]
                        }
                    }
                },
                partitionBy: "$part"
            }
        })");

    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(parsedStage);
    std::vector<Document> docs;
    docs.push_back(DOC("num" << 3 << "val" << 25 << "part" << 1));
    docs.push_back(DOC("num" << 15 << "val" << 12 << "part" << 1));
    docs.push_back(DOC("num" << 2 << "val" << 1 << "part" << 2));
    docs.push_back(DOC("num" << 4 << "val" << 3 << "part" << 2));
    auto mockStage = exec::agg::MockStage::createForTest(docs, getExpCtx());
    stage->setSource(mockStage.get());

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(), "{num: 3, val: 25, part: 1, sum: 37}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(), "{num: 15, val: 12, part: 1, sum: 37}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(), "{num: 2, val: 1, part: 2, sum: 4}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(), "{num: 4, val: 3, part: 2, sum: 4}");
}

TEST_F(DocumentSourceSetWindowFieldsTest, OptimizationRemovesRedundantSortStage) {
    auto swfSpec = fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$y', sortBy: {y: 1}, output: {'x':
        {$sum: 1}}}})");
    auto swfStage =
        DocumentSourceInternalSetWindowFields::createFromBson(swfSpec.firstElement(), getExpCtx());
    auto sortSpec = fromjson(R"({$sort: {y: 1}})");
    auto sortStage = DocumentSourceSort::createFromBson(sortSpec.firstElement(), getExpCtx());
    auto prevSortStage = DocumentSourceSort::createFromBson(sortSpec.firstElement(), getExpCtx());
    DocumentSourceContainer pipeline = {prevSortStage, swfStage, sortStage};

    DocumentSourceContainer::iterator itr = pipeline.begin();

    // We only care about optimizing the setWindowFields stage.
    itr = std::next(itr);
    itr = (*itr).get()->optimizeAt(itr, &pipeline);

    // We should have removed the redundant sort. This optimization works because the preceding and
    // succeeding sorts are the same, and setWindowFields does not change document order.
    ASSERT_EQ(pipeline.size(), 2);
    ASSERT_EQ(std::string(pipeline.front()->getSourceName()), "$sort"_sd);
    ASSERT_EQ(std::string(pipeline.back()->getSourceName()), "$_internalSetWindowFields"_sd);
}

PlanSummaryStats collectPipelineStats(const exec::agg::Pipeline& execPipeline) {
    PlanSummaryStats stats;
    execPipeline.accumulatePlanSummaryStats(stats);
    return stats;
}

TEST_F(DocumentSourceSetWindowFieldsSpillingTest,
       CanSpillAndFailIfCannotSpillAndExceedMemoryLimit) {
    RAIIServerParameterControllerForTest maxMemoryBytes(
        "internalDocumentSourceSetWindowFieldsMaxMemoryBytes", 3000);

    auto wfSpec = fromjson(R"({
            $setWindowFields: {
                sortBy: {val: 1},
                output: {
                    sum: {
                        $sum: "$val",
                        "window": {
                            "documents": [
                                -1000,
                                +1000
                            ]
                        }
                    }
                }
            }
        })");

    std::vector<Document> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(DOC("val" << i));
    }

    for (bool allowDiskUse : {false, true}) {
        getExpCtx()->setAllowDiskUse(allowDiskUse);

        auto pipelineStages =
            document_source_set_window_fields::createFromBson(wfSpec.firstElement(), getExpCtx());
        auto source = DocumentSourceMock::createForTest(docs, getExpCtx());
        pipelineStages.push_front(source);
        auto pipeline = Pipeline::create(pipelineStages, getExpCtx());
        auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

        auto exhaustPipeline = [&]() {
            while (execPipeline->getNext().has_value()) {
            }
        };

        if (allowDiskUse) {
            exhaustPipeline();

            auto planSummaryStats = collectPipelineStats(*execPipeline);
            const auto& spillingStats =
                planSummaryStats
                    .spillingStatsPerStage[PlanSummaryStats::SpillingStage::SET_WINDOW_FIELDS];
            // Exact numbers of spills and bytes might differ on different platforms.
            ASSERT_GTE(spillingStats.getSpills(), 1);
            ASSERT_GTE(spillingStats.getSpilledRecords(), 90);
            ASSERT_GTE(spillingStats.getSpilledBytes(), 5000);
        } else {
            ASSERT_THROWS_CODE(exhaustPipeline(), DBException, 5643011);
        }
    }
}

TEST_F(DocumentSourceSetWindowFieldsSpillingTest, CanForceSpill) {
    auto wfSpec = fromjson(R"({
            $setWindowFields: {
                sortBy: {val: 1},
                output: {
                    sum: {
                        $sum: "$val",
                        "window": {
                            "documents": [
                                -1000,
                                +1000
                            ]
                        }
                    }
                }
            }
        })");

    std::vector<Document> docs;
    for (int i = 0; i < 100; ++i) {
        docs.push_back(DOC("val" << i));
    }

    getExpCtx()->setAllowDiskUse(true);

    auto pipelineStages =
        document_source_set_window_fields::createFromBson(wfSpec.firstElement(), getExpCtx());
    auto source = DocumentSourceMock::createForTest(docs, getExpCtx());
    pipelineStages.push_front(source);
    auto pipeline = Pipeline::create(pipelineStages, getExpCtx());
    auto execPipeline = exec::agg::buildPipeline(pipeline->freeze());

    int nextDocIndex = 0;
    auto assertNextDocument = [&](const boost::optional<Document>& doc) {
        ASSERT_TRUE(doc.has_value());
        ASSERT_EQ(doc->getField("val").coerceToInt(), nextDocIndex++);
        ASSERT_EQ(doc->getField("sum").coerceToInt(), 4950);
    };

    for (int i = 0; i < 20; ++i) {
        assertNextDocument(execPipeline->getNext());
    }

    execPipeline->forceSpill();

    while (auto next = execPipeline->getNext()) {
        assertNextDocument(next);
    };
    ASSERT_EQ(nextDocIndex, docs.size());

    SpillingStats spillingStats =
        collectPipelineStats(*execPipeline)
            .spillingStatsPerStage[PlanSummaryStats::SpillingStage::SET_WINDOW_FIELDS];
    ASSERT_EQ(spillingStats.getSpills(), 1);
    ASSERT_EQ(spillingStats.getSpilledRecords(), 80);
    // Exact number of bytes might differ on different platforms.
    ASSERT_GTE(spillingStats.getSpilledBytes(), 5000);
}

TEST_F(DocumentSourceSetWindowFieldsTest, outputFieldsIsDeterministic) {
    // This test asserts that setWindowFields returns outputFields in the same order for every
    // document. In this instance, each document should have a obj.str field followed by a obj.date
    // field followed by a obj.totalB.

    auto spec = fromjson(  // NOLINT
        R"({
            $setWindowFields: {
                sortBy: { "obj.num": 1 },
                output: {
                    "obj.str": {
                        $max: {$toLower: "$title"}
                    },
                    "obj.obj.date": {
                        $first: { $max: [new Date(00010101), { $dateTrunc: { date: ObjectId('507f191e810c19729de860ea'), unit: "millisecond", timezone: "Australia/Brisbane", startOfWeek: "TUe" }}]}
                    },
                    "obj.totalB": {
                        $sum: "$b"
                    }
                }
            }
        })");
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(spec.firstElement(), getExpCtx());
    auto stage = exec::agg::buildStage(parsedStage);
    const auto mock = DocumentSourceMock::createForTest({}, getExpCtx());
    auto mockedStage = exec::agg::MockStage::createForTest(
        {"{b: 6}", "{b: 5000}", "{b: 50}", "{b: 88}", "{b: 100}"}, getExpCtx());
    stage->setSource(mockedStage.get());

    auto next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(),
              "{b: 6, obj: {str: \"\", obj: {date: 2012-10-17T20:46:22.000Z}, totalB: 5244}}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(),
              "{b: 5000, obj: {str: \"\", obj: {date: 2012-10-17T20:46:22.000Z}, totalB: 5244}}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(),
              "{b: 50, obj: {str: \"\", obj: {date: 2012-10-17T20:46:22.000Z}, totalB: 5244}}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(),
              "{b: 88, obj: {str: \"\", obj: {date: 2012-10-17T20:46:22.000Z}, totalB: 5244}}");

    next = stage->getNext();
    ASSERT(next.isAdvanced());
    ASSERT_EQ(next.getDocument().toString(),
              "{b: 100, obj: {str: \"\", obj: {date: 2012-10-17T20:46:22.000Z}, totalB: 5244}}");
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
                            "alpha": "?number",
                            "input": "$HASH<y>"
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
                            "n": "?number",
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
                                "?number"
                            ],
                            "unit": "month"
                        }
                    }
                }
            }
        })",
        redact(*docSource));
}

/**
 * Helper function that parses the $setWindowFields aggregation stage from the input, serializes it
 * to its representative shape, re-parses the representative shape, and compares to the original.
 */
void assertRepresentativeShapeIsStable(auto expCtx,
                                       BSONObj inputStage,
                                       BSONObj expectedRepresentativeStage) {
    auto parsedStage =
        DocumentSourceInternalSetWindowFields::createFromBson(inputStage.firstElement(), expCtx);
    std::vector<Value> serialization;
    auto opts = SerializationOptions{LiteralSerializationPolicy::kToRepresentativeParseableValue};
    parsedStage->serializeToArray(serialization, opts);

    auto serializedStage = serialization[0].getDocument().toBson();
    ASSERT_BSONOBJ_EQ(serializedStage, expectedRepresentativeStage);

    auto roundTripped = DocumentSourceInternalSetWindowFields::createFromBson(
        serializedStage.firstElement(), expCtx);

    std::vector<Value> newSerialization;
    roundTripped->serializeToArray(newSerialization, opts);
    ASSERT_EQ(newSerialization.size(), 1UL);
    ASSERT_VALUE_EQ(newSerialization[0], serialization[0]);
}

TEST_F(DocumentSourceSetWindowFieldsTest, RoundTripSerializationDocumentWindowBounds) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: [-10, 10]}}}}})"),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {documents: [0, 1]}}}}})"));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RoundTripSerializationRangeWindowBounds) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {range: [-10, 10]}}}}})"),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {range: [0, 1]}}}}})"));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RoundTripSerializationRangeWindowBoundsWithUnit) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {range: [-10, 10], unit: 'second'}}}}})"),
                                      fromjson(R"(
        {$_internalSetWindowFields: {partitionBy: '$state', sortBy: {city: 1}, output: {mySum:
        {$sum: '$pop', window: {range: [0, 1], unit: 'second'}}}}})"));
}

TEST_F(DocumentSourceSetWindowFieldsTest, RoundTripSerializationExpMovingAvg) {
    assertRepresentativeShapeIsStable(getExpCtx(),
                                      fromjson(
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
        })"),
                                      fromjson(
                                          R"({
            $_internalSetWindowFields: {
                partitionBy: '$foo.bar',
                sortBy: {
                    bar: 1
                },
                output: {
                    x: {
                        $expMovingAvg: {
                            alpha: 0.1,
                            input: '$y'
                        }
                    }
                }
            }
        })"));
}

}  // namespace
}  // namespace mongo
