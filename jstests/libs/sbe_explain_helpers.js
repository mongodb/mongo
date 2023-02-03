/**
 * Helpers for checking correctness of generated SBE plans when expected explain() output differs.
 */

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");

function isIdIndexScan(db, root, expectedParentStageForIxScan) {
    const parentStage = getPlanStage(root, expectedParentStageForIxScan);
    if (!parentStage)
        return false;

    const ixscan = parentStage.inputStage;
    if (!ixscan)
        return false;

    return ixscan.stage === "IXSCAN" && !ixscan.hasOwnProperty("filter") &&
        ixscan.indexName === "_id_";
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('queryLayerOutput'),
 * returns all sub-documents whose stage is 'stage'. This can be a SBE stage name like "nlj" or
 * "hash_lookup".
 *
 * Returns an empty array if the plan does not have the requested stage. Asserts that agg explain
 * structure matches expected format.
 */
function getSbePlanStages(queryLayerOutput, stage) {
    assert(queryLayerOutput);
    const queryInfo = getQueryInfoAtTopLevelOrFirstStage(queryLayerOutput);
    // If execution stats are available, then use the execution stats tree.
    if (queryInfo.hasOwnProperty("executionStats")) {
        assert(queryInfo.executionStats.hasOwnProperty("executionStages"), queryInfo);
        return getPlanStages(queryInfo.executionStats.executionStages, stage);
    }

    // Otherwise, we won't extract from the 'queryPlanner' for now.
    return [];
}

/**
 * Gets the query info object at either the top level or the first stage from a v2
 * explainOutput. If a query is a find query or some prefix stage(s) of a pipeline is pushed down to
 * SBE, then plan information will be in the 'queryPlanner' object. Currently, this supports find
 * query or pushed-down prefix pipeline stages.
 */
function getQueryInfoAtTopLevelOrFirstStage(explainOutputV2) {
    if (explainOutputV2.hasOwnProperty("queryPlanner")) {
        return explainOutputV2;
    }

    if (explainOutputV2.hasOwnProperty("stages") && Array.isArray(explainOutputV2.stages) &&
        explainOutputV2.stages.length > 0 && explainOutputV2.stages[0].hasOwnProperty("$cursor") &&
        explainOutputV2.stages[0].$cursor.hasOwnProperty("queryPlanner")) {
        return explainOutputV2.stages[0].$cursor;
    }

    if (explainOutputV2.hasOwnProperty("shards")) {
        for (const shardName in explainOutputV2.shards) {
            const shardExplainOutputV2 = explainOutputV2.shards[shardName];
            return getQueryInfoAtTopLevelOrFirstStage(shardExplainOutputV2);
        }
    }

    assert(false, `expected version 2 explain output but got ${JSON.stringify(explainOutputV2)}`);
    return undefined;
}
