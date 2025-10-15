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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_source_limit.h"
#include "mongo/db/pipeline/document_source_skip.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::rule_based_rewrites::pipeline {
namespace {

using PipelineRewriteEngineTest = AggregationContextFixture;

#define REGISTER_TEST_RULES(DS, ...)                                                             \
    {                                                                                            \
        auto* service = getExpCtx()->getOperationContext()->getServiceContext();                 \
        registration_detail::enforceUniqueRuleNames(service, {__VA_ARGS__});                     \
        getDocumentSourceVisitorRegistry(service)                                                \
            .registerVisitorFunc<registration_detail::RuleRegisteringVisitorCtx, DS>(            \
                [](DocumentSourceVisitorContextBase* ctx, const DocumentSource&) {               \
                    static_cast<registration_detail::RuleRegisteringVisitorCtx*>(ctx)->addRules( \
                        {__VA_ARGS__});                                                          \
                });                                                                              \
    }

void runTest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
             std::vector<StringData> input,
             std::vector<StringData> expected) {
    auto makePipeline = [&](std::vector<StringData> stages) {
        std::vector<BSONObj> bsonStages;
        for (auto&& stage : stages) {
            bsonStages.push_back(fromjson(stage));
        }
        return Pipeline::parse(bsonStages, expCtx);
    };

    auto pipeline = makePipeline(input);
    PipelineRewriteEngine engine{{*pipeline},
                                 static_cast<size_t>(internalQueryMaxPipelineRewrites.load())};

    engine.applyRules();

    Value actualPipeline{pipeline->serializeToBson()};
    Value expectedPipeline{makePipeline(expected)->serializeToBson()};

    ASSERT_BSONOBJ_EQ(BSON_ARRAY(actualPipeline), BSON_ARRAY(expectedPipeline));
}

bool shouldNeverRun(PipelineRewriteContext&) {
    MONGO_UNREACHABLE;
}

bool alwaysFalse(PipelineRewriteContext&) {
    return false;
}

TEST_F(PipelineRewriteEngineTest, RespectPrecondition) {
    REGISTER_TEST_RULES(DocumentSourceLimit, {"CRASH_WHEN_RUN", alwaysFalse, shouldNeverRun, 1.0});

    runTest(getExpCtx(), {"{$limit: 1}"}, {"{$limit: 1}"});
}

TEST_F(PipelineRewriteEngineTest, RespectType) {
    REGISTER_TEST_RULES(DocumentSourceLimit, {"CRASH_WHEN_RUN", alwaysTrue, shouldNeverRun, 1.0});

    runTest(getExpCtx(), {"{$match: {a: 1}}"}, {"{$match: {a: 1}}"});
}

TEST_F(PipelineRewriteEngineTest, RespectPriority) {
    REGISTER_TEST_RULES(DocumentSourceMatch,
                        {"CRASH_WHEN_RUN", alwaysTrue, shouldNeverRun, 1.0},
                        {"REMOVE_MATCH", alwaysTrue, erase, 2.0});

    runTest(getExpCtx(), {"{$match: {a: 1}}"}, {});
}

TEST_F(PipelineRewriteEngineTest, RespectMaxRewritesQueryKnob) {
    RAIIServerParameterControllerForTest controller("internalQueryMaxPipelineRewrites", 2);

    REGISTER_TEST_RULES(DocumentSourceMatch,
                        {"CRASH_WHEN_RUN", alwaysTrue, shouldNeverRun, 1.0},
                        {"NOOP1", alwaysTrue, noop, 2.0},
                        {"NOOP2", alwaysTrue, noop, 3.0});

    runTest(getExpCtx(), {"{$match: {a: 1}}"}, {"{$match: {a: 1}}"});
}

TEST_F(PipelineRewriteEngineTest, ApplySingleRuleInPlace) {
    static auto setLimitTo1 = [](PipelineRewriteContext& ctx) {
        ctx.currentAs<DocumentSourceLimit>().setLimit(1);
        return false;
    };
    REGISTER_TEST_RULES(DocumentSourceLimit, {"SET_LIMIT_TO_1", alwaysTrue, setLimitTo1, 1.0});

    runTest(getExpCtx(), {"{$limit: 10}"}, {"{$limit: 1}"});
}

TEST_F(PipelineRewriteEngineTest, EraseFirstStage) {
    REGISTER_TEST_RULES(DocumentSourceMatch, {"REMOVE_MATCH", alwaysTrue, erase, 1.0});

    runTest(getExpCtx(),
            {
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
            },
            {"{$sort: {a: 1}}"});
}

TEST_F(PipelineRewriteEngineTest, EraseLastStage) {
    REGISTER_TEST_RULES(DocumentSourceSort, {"REMOVE_SORT", alwaysTrue, erase, 1.0});

    runTest(getExpCtx(),
            {
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
            },
            {"{$match: {a: 1}}"});
}

TEST_F(PipelineRewriteEngineTest, EraseNextStage) {
    REGISTER_TEST_RULES(DocumentSourceMatch, {"REMOVE_NEXT", alwaysTrue, eraseNext, 1.0});

    runTest(getExpCtx(),
            {
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
            },
            {"{$match: {a: 1}}"});
}

TEST_F(PipelineRewriteEngineTest, InsertStageBeforeCurrent) {
    static auto insertMatchBefore = [](PipelineRewriteContext& ctx) {
        auto filter = ctx.currentAs<DocumentSourceSort>()
                          .getSortKeyPattern()
                          .serialize(SortPattern::SortKeySerialization::kForPipelineSerialization)
                          .toBson();
        auto matchStage = DocumentSourceMatch::create(filter, ctx.current().getExpCtx());
        return insertBefore(ctx, *matchStage);
    };

    REGISTER_TEST_RULES(DocumentSourceSort,
                        {"INSERT_MATCH_BEFORE_SORT", alwaysTrue, insertMatchBefore, 1.0});

    runTest(getExpCtx(),
            {
                "{$sort: {a: 1}}",
                "{$sort: {b: 1}}",
            },
            {
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
                "{$match: {b: 1}}",
                "{$sort: {b: 1}}",
            });
}

TEST_F(PipelineRewriteEngineTest, PushStageToFront) {
    static auto prevIsNotSort = [](PipelineRewriteContext& ctx) {
        return !ctx.atFirstStage() &&
            ctx.prevStage()->getSourceName() != DocumentSourceSort::kStageName;
    };

    REGISTER_TEST_RULES(DocumentSourceSort,
                        {"SWAP_SORT_WITH_PREV", prevIsNotSort, swapStageWithPrev, 1.0});

    runTest(getExpCtx(),
            {
                "{$sort: {a: 1}}",
                "{$addFields: {a: 1}}",
                "{$sort: {b: 1}}",
                "{$addFields: {b: 1}}",
                "{$match: {a: 1}}",
                "{$sort: {c: 1}}",
            },
            {
                "{$sort: {a: 1}}",
                "{$sort: {b: 1}}",
                "{$sort: {c: 1}}",
                "{$addFields: {a: 1}}",
                "{$addFields: {b: 1}}",
                "{$match: {a: 1}}",
            });
}

TEST_F(PipelineRewriteEngineTest, PushStageToBack) {
    static auto nextIsNotSort = [](PipelineRewriteContext& ctx) {
        return !ctx.atLastStage() &&
            ctx.nextStage()->getSourceName() != DocumentSourceSort::kStageName;
    };

    REGISTER_TEST_RULES(DocumentSourceSort,
                        {"SWAP_SORT_WITH_NEXT", nextIsNotSort, swapStageWithNext, 1.0});

    runTest(getExpCtx(),
            {
                "{$sort: {a: 1}}",
                "{$addFields: {a: 1}}",
                "{$sort: {b: 1}}",
                "{$addFields: {b: 1}}",
                "{$match: {a: 1}}",
                "{$sort: {c: 1}}",
            },
            {
                "{$addFields: {a: 1}}",
                "{$addFields: {b: 1}}",
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
                "{$sort: {b: 1}}",
                "{$sort: {c: 1}}",
            });
}

TEST_F(PipelineRewriteEngineTest, EnqueueAdditionalRulesFromPrecondition) {
    static auto swapIfFirstStage = [](PipelineRewriteContext& ctx) {
        if (ctx.atFirstStage()) {
            ctx.addRules({{"SWAP_WITH_NEXT", alwaysTrue, swapStageWithNext, 2.0}});
            return true;
        }
        return false;
    };

    static auto insertLimit = [](PipelineRewriteContext& ctx) {
        auto limitStage = DocumentSourceLimit::create(ctx.current().getExpCtx(), 10);
        insertAfter(ctx, *limitStage);
        return false;
    };

    REGISTER_TEST_RULES(DocumentSourceSort,
                        {"INSERT_LIMIT_THEN_SWAP", swapIfFirstStage, insertLimit, 1.0});

    runTest(getExpCtx(),
            {
                "{$sort: {a: 1}}",
            },
            {
                "{$limit: 10}",
                "{$sort: {a: 1}}",
            });
}

TEST_F(PipelineRewriteEngineTest, EnqueueAdditionalRulesFromTransform) {
    static auto isFirstStage = [](PipelineRewriteContext& ctx) {
        return ctx.atFirstStage();
    };

    static auto insertLimitThenSwap = [](PipelineRewriteContext& ctx) {
        ctx.addRules({{"SWAP_WITH_NEXT", alwaysTrue, swapStageWithNext, 2.0}});

        auto limitStage = DocumentSourceLimit::create(ctx.current().getExpCtx(), 10);
        insertAfter(ctx, *limitStage);
        return false;
    };

    REGISTER_TEST_RULES(DocumentSourceSort,
                        {"INSERT_LIMIT_THEN_SWAP", isFirstStage, insertLimitThenSwap, 1.0});

    runTest(getExpCtx(),
            {
                "{$sort: {a: 1}}",
            },
            {
                "{$limit: 10}",
                "{$sort: {a: 1}}",
            });
}

TEST_F(PipelineRewriteEngineTest, ApplyMultipleRulesDifferentTypesDifferentPriorities) {
    // Preconditions
    static auto isLimit10 = [](PipelineRewriteContext& ctx) {
        return ctx.currentAs<DocumentSourceLimit>().getLimit() == 10;
    };
    static auto nextIsLimit = [](PipelineRewriteContext& ctx) {
        return !ctx.atFirstStage() &&
            ctx.nextStage()->getSourceName() == DocumentSourceLimit::kStageName;
    };
    static auto betweenMatchAndSort = [](PipelineRewriteContext& ctx) {
        return !ctx.atFirstStage() && !ctx.atLastStage() &&
            ctx.prevStage()->getSourceName() == DocumentSourceMatch::kStageName &&
            ctx.nextStage()->getSourceName() == DocumentSourceSort::kStageName;
    };

    // Transforms
    static auto insertSkipAfter = [](PipelineRewriteContext& ctx) {
        auto skipStage = DocumentSourceSkip::create(ctx.current().getExpCtx(), 5);
        return insertAfter(ctx, *skipStage);
    };
    static auto insertLimitAfter = [](PipelineRewriteContext& ctx) {
        auto limitStage = DocumentSourceLimit::create(ctx.current().getExpCtx(), 10);
        return insertAfter(ctx, *limitStage);
    };
    static auto setLimitTo1 = [](PipelineRewriteContext& ctx) {
        ctx.currentAs<DocumentSourceLimit>().setLimit(1);
        return false;
    };

    REGISTER_TEST_RULES(
        DocumentSourceMatch,
        {"INSERT_SKIP_AFTER_MATCH_ON_A", betweenMatchAndSort, insertSkipAfter, 1.0});
    REGISTER_TEST_RULES(DocumentSourceSkip,
                        {"INSERT_LIMIT_AFTER_SKIP", betweenMatchAndSort, insertLimitAfter, 2.0},
                        {"SWAP_WITH_LIMIT", nextIsLimit, swapStageWithNext, 1.0});
    REGISTER_TEST_RULES(DocumentSourceLimit,
                        {"SET_LIMIT_FROM_10_TO_1", isLimit10, setLimitTo1, 2.0},
                        {"CRASH_WHEN_RUN", isLimit10, shouldNeverRun, 1.0});

    runTest(getExpCtx(),
            {
                "{$match: {b: 1}}",
                "{$match: {a: 1}}",
                "{$sort: {a: 1}}",
            },
            {
                "{$match: {b: 1}}",
                "{$match: {a: 1}}",
                "{$limit: 1}",
                "{$skip: 5}",
                "{$sort: {a: 1}}",
            });
}

DEATH_TEST_F(PipelineRewriteEngineTest, FailsOnDuplicateRuleNames, "11010016") {
    REGISTER_TEST_RULES(DocumentSourceMatch, {"DUPLICATE_RULE_NAME", alwaysTrue, noop, 1.0});
    REGISTER_TEST_RULES(DocumentSourceSort, {"DUPLICATE_RULE_NAME", alwaysTrue, noop, 1.0});
}

}  // namespace
}  // namespace mongo::rule_based_rewrites::pipeline
