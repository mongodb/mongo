// Contains helpers for checking, based on the explain output, properties of a
// plan. For instance, there are helpers for checking whether a plan is a collection
// scan or whether the plan is covered (index only).

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. Returns an empty array if the plan does not have the
 * requested stage.
 */
function getPlanStages(root, stage) {
    var results = [];

    if (root.stage === stage) {
        results.push(root);
    }

    if ("inputStage" in root) {
        results = results.concat(getPlanStages(root.inputStage, stage));
    }

    if ("inputStages" in root) {
        for (var i = 0; i < root.inputStages.length; i++) {
            results = results.concat(getPlanStages(root.inputStages[i], stage));
        }
    }

    if ("shards" in root) {
        for (var i = 0; i < root.shards.length; i++) {
            if ("winningPlan" in root.shards[i]) {
                results = results.concat(getPlanStages(root.shards[i].winningPlan, stage));
            } else {
                results = results.concat(getPlanStages(root.shards[i].executionStages, stage));
            }
        }
    }

    return results;
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 */
function getPlanStage(root, stage) {
    var planStageList = getPlanStages(root, stage);

    if (planStageList.length === 0) {
        return null;
    } else {
        assert(planStageList.length === 1,
               "getPlanStage expects to find 0 or 1 matching stages. planStageList: " +
                   tojson(planStageList));
        return planStageList[0];
    }
}

/**
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan has a stage called 'stage'.
 */
function planHasStage(root, stage) {
    return getPlanStage(root, stage) !== null;
}

/**
 * A query is covered iff it does *not* have a FETCH stage or a COLLSCAN.
 *
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan is index only. Otherwise returns false.
 */
function isIndexOnly(root) {
    return !planHasStage(root, "FETCH") && !planHasStage(root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * an index scan, and false otherwise.
 */
function isIxscan(root) {
    return planHasStage(root, "IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the idhack fast path, and false otherwise.
 */
function isIdhack(root) {
    return planHasStage(root, "IDHACK");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a collection scan, and false otherwise.
 */
function isCollscan(root) {
    return planHasStage(root, "COLLSCAN");
}

/**
 * Get the number of chunk skips for the BSON exec stats tree rooted at 'root'.
 */
function getChunkSkips(root) {
    if (root.stage === "SHARDING_FILTER") {
        return root.chunkSkips;
    } else if ("inputStage" in root) {
        return getChunkSkips(root.inputStage);
    } else if ("inputStages" in root) {
        var skips = 0;
        for (var i = 0; i < root.inputStages.length; i++) {
            skips += getChunkSkips(root.inputStages[0]);
        }
        return skips;
    }

    return 0;
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. This can either be an agg stage name like "$cursor" or
 * "$sort", or a query stage name like "IXSCAN" or "SORT".
 *
 * Returns an empty array if the plan does not have the requested stage. Asserts that agg explain
 * structure matches expected format.
 */
function getAggPlanStages(root, stage) {
    let results = [];

    function getDocumentSources(docSourceArray) {
        let results = [];
        for (let i = 0; i < docSourceArray.length; i++) {
            let properties = Object.getOwnPropertyNames(docSourceArray[i]);
            assert.eq(1, properties.length);
            if (properties[0] === stage) {
                results.push(docSourceArray[i]);
            }
        }
        return results;
    }

    if (root.hasOwnProperty("stages")) {
        assert(root.stages.constructor === Array);

        results = results.concat(getDocumentSources(root.stages));

        assert(root.stages[0].hasOwnProperty("$cursor"));
        assert(root.stages[0].$cursor.hasOwnProperty("queryPlanner"));
        assert(root.stages[0].$cursor.queryPlanner.hasOwnProperty("winningPlan"));
        results =
            results.concat(getPlanStages(root.stages[0].$cursor.queryPlanner.winningPlan, stage));
    }

    if (root.hasOwnProperty("shards")) {
        for (let elem in root.shards) {
            assert(root.shards[elem].stages.constructor === Array);

            results = results.concat(getDocumentSources(root.shards[elem].stages));

            assert(root.shards[elem].stages[0].hasOwnProperty("$cursor"));
            assert(root.shards[elem].stages[0].$cursor.hasOwnProperty("queryPlanner"));
            assert(root.shards[elem].stages[0].$cursor.queryPlanner.hasOwnProperty("winningPlan"));
            results = results.concat(
                getPlanStages(root.shards[elem].stages[0].$cursor.queryPlanner.winningPlan, stage));
        }
    }

    return results;
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 */
function getAggPlanStage(root, stage) {
    let planStageList = getAggPlanStages(root, stage);

    if (planStageList.length === 0) {
        return null;
    } else {
        assert.eq(1,
                  planStageList.length,
                  "getAggPlanStage expects to find 0 or 1 matching stages. planStageList: " +
                      tojson(planStageList));
        return planStageList[0];
    }
}
