// Contains helpers for checking, based on the explain output, properties of a
// plan. For instance, there are helpers for checking whether a plan is a collection
// scan or whether the plan is covered (index only).

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

/**
 * Returns a sub-element of the 'queryPlanner' explain output which represents a winning plan.
 */
function getWinningPlan(queryPlanner) {
    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present,
    // it will hold a serialized winning plan, otherwise it will be stored in the 'winningPlan'
    // field itself.
    return queryPlanner.winningPlan.hasOwnProperty("queryPlan") ? queryPlanner.winningPlan.queryPlan
                                                                : queryPlanner.winningPlan;
}

/**
 * Returns an element of explain output which represents a rejected candidate plan.
 */
function getRejectedPlan(rejectedPlan) {
    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present,
    // it will hold a serialized winning plan, otherwise it will be stored in the 'rejectedPlan'
    // element itself.
    return rejectedPlan.hasOwnProperty("queryPlan") ? rejectedPlan.queryPlan : rejectedPlan;
}

/**
 * Returns a sub-element of the 'cachedPlan' explain output which represents a query plan.
 */
function getCachedPlan(cachedPlan) {
    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present, it
    // will hold a serialized cached plan, otherwise it will be stored in the 'cachedPlan' field
    // itself.
    return cachedPlan.hasOwnProperty("queryPlan") ? cachedPlan.queryPlan : cachedPlan;
}

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

    if ("queryPlanner" in root) {
        results = results.concat(getPlanStages(getWinningPlan(root.queryPlanner), stage));
    }

    if ("thenStage" in root) {
        results = results.concat(getPlanStages(root.thenStage, stage));
    }

    if ("elseStage" in root) {
        results = results.concat(getPlanStages(root.elseStage, stage));
    }

    if ("outerStage" in root) {
        results = results.concat(getPlanStages(root.outerStage, stage));
    }

    if ("innerStage" in root) {
        results = results.concat(getPlanStages(root.innerStage, stage));
    }

    if ("shards" in root) {
        if (Array.isArray(root.shards)) {
            results =
                root.shards.reduce((res, shard) => res.concat(getPlanStages(
                                       shard.hasOwnProperty("winningPlan") ? getWinningPlan(shard)
                                                                           : shard.executionStages,
                                       stage)),
                                   results);
        } else {
            const shards = Object.keys(root.shards);
            results = shards.reduce(
                (res, shard) => res.concat(getPlanStages(root.shards[shard], stage)), results);
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
 * Returns the set of rejected plans from the given replset or sharded explain output.
 */
function getRejectedPlans(root) {
    if (root.queryPlanner.winningPlan.hasOwnProperty("shards")) {
        const rejectedPlans = [];
        for (let shard of root.queryPlanner.winningPlan.shards) {
            for (let rejectedPlan of shard.rejectedPlans) {
                rejectedPlans.push(Object.assign({shardName: shard.shardName}, rejectedPlan));
            }
        }
        return rejectedPlans;
    }
    return root.queryPlanner.rejectedPlans;
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns true if
 * the query planner reports at least one rejected alternative plan, and false otherwise.
 */
function hasRejectedPlans(root) {
    function sectionHasRejectedPlans(explainSection) {
        assert(explainSection.hasOwnProperty("rejectedPlans"), tojson(explainSection));
        return explainSection.rejectedPlans.length !== 0;
    }

    function cursorStageHasRejectedPlans(cursorStage) {
        assert(cursorStage.hasOwnProperty("$cursor"), tojson(cursorStage));
        assert(cursorStage.$cursor.hasOwnProperty("queryPlanner"), tojson(cursorStage));
        return sectionHasRejectedPlans(cursorStage.$cursor.queryPlanner);
    }

    if (root.hasOwnProperty("shards")) {
        // This is a sharded agg explain. Recursively check whether any of the shards has rejected
        // plans.
        const shardExplains = [];
        for (const shard in root.shards) {
            shardExplains.push(root.shards[shard]);
        }
        return shardExplains.some(hasRejectedPlans);
    } else if (root.hasOwnProperty("stages")) {
        // This is an agg explain.
        const cursorStages = getAggPlanStages(root, "$cursor");
        return cursorStages.find((cursorStage) => cursorStageHasRejectedPlans(cursorStage)) !==
            undefined;
    } else {
        // This is some sort of query explain.
        assert(root.hasOwnProperty("queryPlanner"), tojson(root));
        assert(root.queryPlanner.hasOwnProperty("winningPlan"), tojson(root));
        if (!root.queryPlanner.winningPlan.hasOwnProperty("shards")) {
            // This is an unsharded explain.
            return sectionHasRejectedPlans(root.queryPlanner);
        }
        // This is a sharded explain. Each entry in the shards array contains a 'winningPlan' and
        // 'rejectedPlans'.
        return root.queryPlanner.winningPlan.shards.find(
                   (shard) => sectionHasRejectedPlans(shard)) !== undefined;
    }
}

/**
 * Returns an array of execution stages from the given replset or sharded explain output.
 */
function getExecutionStages(root) {
    if (root.executionStats.executionStages.hasOwnProperty("shards")) {
        const executionStages = [];
        for (let shard of root.executionStats.executionStages.shards) {
            executionStages.push(Object.assign(
                {shardName: shard.shardName, executionSuccess: shard.executionSuccess},
                shard.executionStages));
        }
        return executionStages;
    }
    return [root.executionStats.executionStages];
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. This can either be an agg stage name like "$cursor" or
 * "$sort", or a query stage name like "IXSCAN" or "SORT".
 *
 * If 'useQueryPlannerSection' is set to 'true', the 'queryPlanner' section of the explain output
 * will be used to lookup the given 'stage', even if 'executionStats' section is available.
 *
 * Returns an empty array if the plan does not have the requested stage. Asserts that agg explain
 * structure matches expected format.
 */
function getAggPlanStages(root, stage, useQueryPlannerSection = false) {
    let results = [];

    function getDocumentSources(docSourceArray) {
        let results = [];
        for (let i = 0; i < docSourceArray.length; i++) {
            let properties = Object.getOwnPropertyNames(docSourceArray[i]);
            if (properties[0] === stage) {
                results.push(docSourceArray[i]);
            }
        }
        return results;
    }

    function getStagesFromQueryLayerOutput(queryLayerOutput) {
        let results = [];

        assert(queryLayerOutput.hasOwnProperty("queryPlanner"));
        assert(queryLayerOutput.queryPlanner.hasOwnProperty("winningPlan"));

        // If execution stats are available, then use the execution stats tree. Otherwise use the
        // plan info from the "queryPlanner" section.
        if (queryLayerOutput.hasOwnProperty("executionStats") && !useQueryPlannerSection) {
            assert(queryLayerOutput.executionStats.hasOwnProperty("executionStages"));
            results = results.concat(
                getPlanStages(queryLayerOutput.executionStats.executionStages, stage));
        } else {
            results =
                results.concat(getPlanStages(getWinningPlan(queryLayerOutput.queryPlanner), stage));
        }

        return results;
    }

    if (root.hasOwnProperty("stages")) {
        assert(root.stages.constructor === Array);

        results = results.concat(getDocumentSources(root.stages));

        if (root.stages[0].hasOwnProperty("$cursor")) {
            results = results.concat(getStagesFromQueryLayerOutput(root.stages[0].$cursor));
        } else if (root.stages[0].hasOwnProperty("$geoNearCursor")) {
            results = results.concat(getStagesFromQueryLayerOutput(root.stages[0].$geoNearCursor));
        }
    }

    if (root.hasOwnProperty("shards")) {
        for (let elem in root.shards) {
            if (root.shards[elem].hasOwnProperty("queryPlanner")) {
                // The shard was able to optimize away the pipeline, which means that the format of
                // the explain output doesn't have the "stages" array.
                assert.eq(true, root.shards[elem].queryPlanner.optimizedPipeline);
                results = results.concat(getStagesFromQueryLayerOutput(root.shards[elem]));

                // Move onto the next shard.
                continue;
            }

            if (!root.shards[elem].hasOwnProperty("stages")) {
                continue;
            }

            assert(root.shards[elem].stages.constructor === Array);

            results = results.concat(getDocumentSources(root.shards[elem].stages));

            const firstStage = root.shards[elem].stages[0];
            if (firstStage.hasOwnProperty("$cursor")) {
                results = results.concat(getStagesFromQueryLayerOutput(firstStage.$cursor));
            } else if (firstStage.hasOwnProperty("$geoNearCursor")) {
                results = results.concat(getStagesFromQueryLayerOutput(firstStage.$geoNearCursor));
            }
        }
    }

    // If the agg pipeline was completely optimized away, then the agg explain output will be
    // formatted like the explain output for a find command.
    if (root.hasOwnProperty("queryPlanner")) {
        assert.eq(true, root.queryPlanner.optimizedPipeline);
        results = results.concat(getStagesFromQueryLayerOutput(root));
    }

    return results;
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 *
 * If 'useQueryPlannerSection' is set to 'true', the 'queryPlanner' section of the explain output
 * will be used to lookup the given 'stage', even if 'executionStats' section is available.
 */
function getAggPlanStage(root, stage, useQueryPlannerSection = false) {
    let planStageList = getAggPlanStages(root, stage, useQueryPlannerSection);

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

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns
 * whether the plan has a stage called 'stage'. It could have more than one to allow for sharded
 * explain plans, and it can search for a query planner stage like "FETCH" or an agg stage like
 * "$group."
 */
function aggPlanHasStage(root, stage) {
    return getAggPlanStages(root, stage).length > 0;
}

/**
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan has a stage called 'stage'.
 */
function planHasStage(db, root, stage) {
    const matchingStages = getPlanStages(root, stage);

    // If we are executing against a mongos, we may get more than one occurrence of the stage.
    if (FixtureHelpers.isMongos(db)) {
        return matchingStages.length >= 1;
    } else {
        assert.lt(matchingStages.length,
                  2,
                  `Expected to find 0 or 1 matching stages: ${tojson(matchingStages)}`);
        return matchingStages.length === 1;
    }
}

/**
 * A query is covered iff it does *not* have a FETCH stage or a COLLSCAN.
 *
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan is index only. Otherwise returns false.
 */
function isIndexOnly(db, root) {
    return !planHasStage(db, root, "FETCH") && !planHasStage(db, root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * an index scan, and false otherwise.
 */
function isIxscan(db, root) {
    return planHasStage(db, root, "IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the idhack fast path, and false otherwise.
 */
function isIdhack(db, root) {
    return planHasStage(db, root, "IDHACK");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a collection scan, and false otherwise.
 */
function isCollscan(db, root) {
    return planHasStage(db, root, "COLLSCAN");
}

function isClusteredIxscan(db, root) {
    return planHasStage(db, root, "CLUSTERED_IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using the aggregation
 * framework, and false otherwise.
 */
function isAggregationPlan(root) {
    if (root.hasOwnProperty("shards")) {
        const shards = Object.keys(root.shards);
        return shards.reduce(
                   (res, shard) => res + root.shards[shard].hasOwnProperty("stages") ? 1 : 0, 0) >
            0;
    }
    return root.hasOwnProperty("stages");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using just the query layer,
 * and false otherwise.
 */
function isQueryPlan(root) {
    if (root.hasOwnProperty("shards")) {
        const shards = Object.keys(root.shards);
        return shards.reduce(
                   (res, shard) => res + root.shards[shard].hasOwnProperty("queryPlanner") ? 1 : 0,
                   0) > 0;
    }
    return root.hasOwnProperty("queryPlanner");
}

/**
 * Get the "chunk skips" for a single shard. Here, "chunk skips" refer to documents excluded by the
 * shard filter.
 */
function getChunkSkipsFromShard(shardPlan, shardExecutionStages) {
    const shardFilterPlanStage = getPlanStage(getWinningPlan(shardPlan), "SHARDING_FILTER");
    if (!shardFilterPlanStage) {
        return 0;
    }

    if (shardFilterPlanStage.hasOwnProperty("planNodeId")) {
        const shardFilterNodeId = shardFilterPlanStage.planNodeId;

        // If the query plan's shard filter has a 'planNodeId' value, we search for the
        // corresponding SBE filter stage and use its stats to determine how many documents were
        // excluded.
        const filters = getPlanStages(shardExecutionStages.executionStages, "filter")
                            .filter(stage => (stage.planNodeId === shardFilterNodeId));
        return filters.reduce((numSkips, stage) => (numSkips + (stage.numTested - stage.nReturned)),
                              0);
    } else {
        // Otherwise, we assume that execution used a "classic" SHARDING_FILTER stage, which
        // explicitly reports a "chunkSkips" value.
        const filters = getPlanStages(shardExecutionStages.executionStages, "SHARDING_FILTER");
        return filters.reduce((numSkips, stage) => (numSkips + stage.chunkSkips), 0);
    }
}

/**
 * Get the sum of "chunk skips" from all shards. Here, "chunk skips" refer to documents excluded by
 * the shard filter.
 */
function getChunkSkipsFromAllShards(explainResult) {
    const shardPlanArray = explainResult.queryPlanner.winningPlan.shards;
    const shardExecutionStagesArray = explainResult.executionStats.executionStages.shards;
    assert.eq(shardPlanArray.length, shardExecutionStagesArray.length, explainResult);

    let totalChunkSkips = 0;
    for (let i = 0; i < shardPlanArray.length; i++) {
        totalChunkSkips += getChunkSkipsFromShard(shardPlanArray[i], shardExecutionStagesArray[i]);
    }
    return totalChunkSkips;
}

/**
 * Given explain output at executionStats level verbosity, confirms that the root stage is COUNT or
 * RECORD_STORE_FAST_COUNT and that the result of the count is equal to 'expectedCount'.
 */
function assertExplainCount({explainResults, expectedCount}) {
    const execStages = explainResults.executionStats.executionStages;

    // If passed through mongos, then the root stage should be the mongos SINGLE_SHARD stage or
    // SHARD_MERGE stages, with COUNT as the root stage on each shard. If explaining directly on the
    // shard, then COUNT is the root stage.
    if ("SINGLE_SHARD" == execStages.stage || "SHARD_MERGE" == execStages.stage) {
        let totalCounted = 0;
        for (let shardExplain of execStages.shards) {
            const countStage = shardExplain.executionStages;
            assert(countStage.stage === "COUNT" || countStage.stage === "RECORD_STORE_FAST_COUNT",
                   `Root stage on shard is not COUNT or RECORD_STORE_FAST_COUNT. ` +
                       `The actual plan is: ${tojson(explainResults)}`);
            totalCounted += countStage.nCounted;
        }
        assert.eq(totalCounted,
                  expectedCount,
                  assert.eq(totalCounted, expectedCount, "wrong count result"));
    } else {
        assert(execStages.stage === "COUNT" || execStages.stage === "RECORD_STORE_FAST_COUNT",
               `Root stage on shard is not COUNT or RECORD_STORE_FAST_COUNT. ` +
                   `The actual plan is: ${tojson(explainResults)}`);
        assert.eq(
            execStages.nCounted,
            expectedCount,
            "Wrong count result. Actual: " + execStages.nCounted + "expected: " + expectedCount);
    }
}

/**
 * Verifies that a given query uses an index and is covered when used in a count command.
 */
function assertCoveredQueryAndCount({collection, query, project, count}) {
    let explain = collection.find(query, project).explain();
    assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)),
           "Winning plan was not covered: " + tojson(explain.queryPlanner.winningPlan));

    // Same query as a count command should also be covered.
    explain = collection.explain("executionStats").find(query).count();
    assert(isIndexOnly(db, getWinningPlan(explain.queryPlanner)),
           "Winning plan for count was not covered: " + tojson(explain.queryPlanner.winningPlan));
    assertExplainCount({explainResults: explain, expectedCount: count});
}

/**
 * Runs explain() operation on 'cmdObj' and verifies that all the stages in 'expectedStages' are
 * present exactly once in the plan returned. When 'stagesNotExpected' array is passed, also
 * verifies that none of those stages are present in the explain() plan.
 */
function assertStagesForExplainOfCommand({coll, cmdObj, expectedStages, stagesNotExpected}) {
    const plan = assert.commandWorked(coll.runCommand({explain: cmdObj}));
    const winningPlan = getWinningPlan(plan.queryPlanner);
    for (let expectedStage of expectedStages) {
        assert(planHasStage(coll.getDB(), winningPlan, expectedStage),
               "Could not find stage " + expectedStage + ". Plan: " + tojson(plan));
    }
    for (let stage of (stagesNotExpected || [])) {
        assert(!planHasStage(coll.getDB(), winningPlan, stage),
               "Found stage " + stage + " when not expected. Plan: " + tojson(plan));
    }
    return plan;
}

/**
 * Get the "planCacheKey" from the explain result.
 */
function getPlanCacheKeyFromExplain(explainRes, db) {
    const hash = FixtureHelpers.isMongos(db) &&
            explainRes.queryPlanner.hasOwnProperty("winningPlan") &&
            explainRes.queryPlanner.winningPlan.hasOwnProperty("shards")
        ? explainRes.queryPlanner.winningPlan.shards[0].planCacheKey
        : explainRes.queryPlanner.planCacheKey;
    assert.eq(typeof hash, "string");

    return hash;
}

/**
 * Helper to run a explain on the given query shape and get the "planCacheKey" from the explain
 * result.
 */
function getPlanCacheKeyFromShape({query = {}, projection = {}, sort = {}, collection, db}) {
    const explainRes =
        assert.commandWorked(collection.explain().find(query, projection).sort(sort).finish());

    return getPlanCacheKeyFromExplain(explainRes, db);
}

/**
 * Helper to run a explain on the given pipeline and get the "planCacheKey" from the explain
 * result.
 */
function getPlanCacheKeyFromPipeline(pipeline, collection, db) {
    const explainRes = assert.commandWorked(collection.explain().aggregate(pipeline));

    return getPlanCacheKeyFromExplain(explainRes, db);
}

/**
 * Given the winning query plan, flatten query plan tree into a list of plan stage names.
 */
function flattenQueryPlanTree(winningPlan) {
    let stages = [];
    while (winningPlan) {
        stages.push(winningPlan.stage);
        winningPlan = winningPlan.inputStage;
    }
    stages.reverse();
    return stages;
}
