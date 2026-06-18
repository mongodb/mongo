/**
 * Tests for dummy and experimental aggregation rewrite rules.
 *
 * @tags: [
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {isAggregationPlan} from "jstests/libs/query/analyze_plan.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {configureFailPointForAllShardsAndMongos} from "jstests/libs/fail_point_util.js";

function setServerParameter(knob, value) {
    setParameterOnAllNonConfigNodes(db.getMongo(), knob, value);
}

if (
    !FeatureFlagUtil.isPresentAndEnabled(db, "featureFlagEnableTestingAggregateRewriteRules") ||
    !FeatureFlagUtil.isPresentAndEnabled(db, "featureFlagPathArrayness")
) {
    jsTest.log.info("Skipping test Feature is not enabled");
    quit();
}

configureFailPointForAllShardsAndMongos({
    conn: db.getMongo(),
    failPointName: "disablePipelineOptimization",
    failPointMode: "off",
});

function explainPipeline(coll, pipeline) {
    const explain = coll.explain().aggregate([{$_internalInhibitOptimization: {}}, ...pipeline]);
    assert.eq(true, isAggregationPlan(explain), "Pipeline should not be optimized away");
    return explain;
}

function isShardedHelper(coll, pipeline) {
    const allStages = explainPipeline(coll, pipeline);
    return FixtureHelpers.isSharded(coll) || "shards" in allStages;
}

const coll = db[jsTestName()];

function reInitializeCollScalarOnly(coll) {
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, test: 1}));
    assert.commandWorked(coll.insert({_id: 1, test: 2}));
    assert.commandWorked(coll.insert({_id: 2, test: 3}));
}

function addArraysToColl(coll) {
    assert.commandWorked(coll.insert({_id: 3, test: [1, 2]}));
    assert.commandWorked(coll.insert({_id: 4, test: [2, 3]}));
    assert.commandWorked(coll.insert({_id: 5, test: [3, 4]}));
}

function runAndReturnStagesFromExplain(coll, query) {
    const beforeAdditionalRule = explainPipeline(coll, query);
    let allShardStages = [];
    if (FixtureHelpers.isSharded(coll) || "shards" in beforeAdditionalRule) {
        for (var shard in beforeAdditionalRule.shards) {
            allShardStages.push(beforeAdditionalRule.shards[shard].stages);
        }
        allShardStages = allShardStages.flat();
    } else {
        if ("stages" in beforeAdditionalRule) {
            allShardStages = beforeAdditionalRule.stages;
        }
    }

    if (Array.isArray(allShardStages)) {
        for (var i = 0; i < allShardStages.length; i++) {
            var obj = allShardStages[i];
            for (var key in obj) {
                if (key == "$facet" && obj[key].hasOwnProperty("originalPipeline")) {
                    allShardStages = obj[key]["originalPipeline"];
                }
            }
        }
    }

    return allShardStages;
}

// 1. Query only scalar with no indexes.
{
    let query = [{$match: {test: 1}}];

    reInitializeCollScalarOnly(coll);

    if (!isShardedHelper(coll, query)) {
        let allStagesBefore = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"test": {"$eq": 1}}},
            allStagesBefore,
            "The $match stage should contain only the original query",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", true);

        let allStagesAfter = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"$and": [{"test": {"$type": [4]}}, {"test": {"$eq": 1}}]}},
            allStagesAfter,
            "The $match stage should be extended with the array type matching",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", false);
    }
}

// 2. Query only scalar with indexes.
{
    let query = [{$match: {test: 1}}];

    reInitializeCollScalarOnly(coll);
    assert.commandWorked(coll.createIndex({test: 1}));

    if (!isShardedHelper(coll, query)) {
        let allStagesBefore = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"test": {"$eq": 1}}},
            allStagesBefore,
            "The $match stage should contain only the original query",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", true);

        let allStagesAfter = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"$and": [{"test": {"$not": {"$type": [4]}}}, {"test": {"$eq": 1}}]}},
            allStagesAfter,
            "The $match stage should be extended with the array type matching",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", false);
    }
}

// 3. Query over both scalar and array value with indexes.
{
    let query = [{$match: {test: 1}}];

    reInitializeCollScalarOnly(coll);
    addArraysToColl(coll);
    assert.commandWorked(coll.createIndex({test: 1}));

    if (!isShardedHelper(coll, query)) {
        let allShardStagesBefore = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"test": {"$eq": 1}}},
            allShardStagesBefore,
            "The $match stage should contain only the original query",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", true);

        let allShardStagesAfter = runAndReturnStagesFromExplain(coll, query);
        assert.contains(
            {"$match": {"$and": [{"test": {"$type": [4]}}, {"test": {"$eq": 1}}]}},
            allShardStagesAfter,
            "The $match stage should be extended with the array type matching",
        );

        setServerParameter("internalEnablePipelineOptimizationAdditionalTestingRules", false);
    }
}
