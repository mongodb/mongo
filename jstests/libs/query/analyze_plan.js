// Contains helpers for checking, based on the explain output, properties of a
// plan. For instance, there are helpers for checking whether a plan is a collection
// scan or whether the plan is covered (index only).

import {documentEq} from "jstests/aggregation/extras/utils.js";

/**
 * Returns query planner part of explain for every node in the explain report.
 */
export function getQueryPlanners(explain) {
    return getAllNodeExplains(explain).flatMap(nodeExplain => {
        // When the shards are present in 'queryPlanner.winningPlan', then the 'nodeExplain' itself
        // represents the shard's 'queryPlanner'.
        const isQueryPlanner = nodeExplain.hasOwnProperty("winningPlan");
        if (isQueryPlanner) {
            return [nodeExplain];
        }
        // Otherwise, the planner outputs will be nested deeper under a 'queryPlanner' field.
        return getNestedProperties(nodeExplain, "queryPlanner");
    });
}

/**
 * Utility to return the 'queryPlanner' section of 'explain'. The input is the root of the explain
 * output.
 */
export function getQueryPlanner(explain) {
    explain = getSingleNodeExplain(explain);
    if ("queryPlanner" in explain) {
        const qp = explain.queryPlanner;
        // Sharded case.
        if ("winningPlan" in qp && "shards" in qp.winningPlan) {
            return qp.winningPlan.shards[0];
        }
        return qp;
    }
    assert(explain.hasOwnProperty("stages"), explain);
    const stage = explain.stages[0];
    assert(stage.hasOwnProperty("$cursor"), explain);
    const cursorStage = stage.$cursor;
    assert(cursorStage.hasOwnProperty("queryPlanner"), explain);
    return cursorStage.queryPlanner;
}

/**
 * Help function to extract shards from explain in sharded environment. Returns null for
 * non-sharded plans.
 */
export function getShardsFromExplain(explain) {
    if (explain.hasOwnProperty("queryPlanner") &&
        explain.queryPlanner.hasOwnProperty("winningPlan")) {
        return explain.queryPlanner.winningPlan.shards;
    }

    return null;
}

/**
 * Extracts and returns an array of explain outputs for every shard in a sharded cluster; returns
 * the original explain output in case of a single replica set.
 */
export function getAllNodeExplains(explain) {
    let shardsExplain = [];

    // If 'splitPipeline' is defined, there could be explains for each shard in the 'mergerPart' of
    // the 'splitPipeline', e.g. $unionWith.
    if (explain.splitPipeline) {
        const splitPipelineShards = getNestedProperties(explain.splitPipeline, "shards");
        shardsExplain.push(...splitPipelineShards.flatMap(Object.values));
    }

    if (explain.shards) {
        shardsExplain.push(...Object.values(explain.shards));
    }

    // NOTE: When shards explain is present in the 'queryPlanner.winningPlan' the shard explains are
    // placed in the array and therefore there is no need to call Object.values() on each element.
    const shards = getShardsFromExplain(explain);

    if (shards) {
        assert(Array.isArray(shards), shards);
        shardsExplain.push(...shards);
    }

    if (shardsExplain.length > 0) {
        return shardsExplain;
    }
    return [explain];
}

/**
 * Returns the output from a single shard if 'explain' was obtained from an unsharded collection;
 * returns 'explain' as is otherwise.
 */
export function getSingleNodeExplain(explain) {
    if ("shards" in explain) {
        const shards = explain.shards;
        const shardNames = Object.keys(shards);
        // There should only be one shard given that this function assumes that 'explain' was
        // obtained from an unsharded collection.
        assert.eq(shardNames.length, 1, explain);
        return shards[shardNames[0]];
    }
    return explain;
}

/**
 * Returns the winning plan from the corresponding sub-node of classic/SBE explain output. Takes
 * into account that the plan may or may not have agg stages.
 * For sharded collections, this may return the top-level "winningPlan" which contains the shards.
 * To ensure getting the winning plan for a specific shard, provide as input the specific explain
 * for that shard i.e, explain.queryPlanner.winningPlan.shards[shardNames[0]].
 */
export function getWinningPlanFromExplain(explain, isSBEPlan = false) {
    let getWinningSBEPlan = (queryPlanner) => queryPlanner.winningPlan.slotBasedPlan;

    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present,
    // it will hold a serialized winning plan, otherwise it will be stored in the 'winningPlan'
    // field itself.
    let getWinningPlan = (queryPlanner) => queryPlanner.winningPlan.hasOwnProperty("queryPlan")
        ? queryPlanner.winningPlan.queryPlan
        : queryPlanner.winningPlan;

    if ("shards" in explain) {
        for (const shardName in explain.shards) {
            let queryPlanner = getQueryPlanner(explain.shards[shardName]);
            return isSBEPlan ? getWinningSBEPlan(queryPlanner) : getWinningPlan(queryPlanner);
        }
    }

    if (explain.hasOwnProperty("pipeline")) {
        const pipeline = explain.pipeline;
        // Pipeline stages' explain output come in two shapes:
        // 1. When in single node, as a single object array
        // 2. When in sharded, as an object.
        if (pipeline.constructor === Array) {
            return getWinningPlanFromExplain(pipeline[0].$cursor, isSBEPlan);
        } else {
            return getWinningPlanFromExplain(pipeline, isSBEPlan);
        }
    }

    let queryPlanner = explain;
    if (explain.hasOwnProperty("queryPlanner") || explain.hasOwnProperty("stages")) {
        queryPlanner = getQueryPlanner(explain);
    }
    return isSBEPlan ? getWinningSBEPlan(queryPlanner) : getWinningPlan(queryPlanner);
}

/**
 * Returns an element of explain output which represents a rejected candidate plan.
 */
export function getRejectedPlan(rejectedPlan) {
    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present,
    // it will hold a serialized winning plan, otherwise it will be stored in the 'rejectedPlan'
    // element itself.
    return rejectedPlan.hasOwnProperty("queryPlan") ? rejectedPlan.queryPlan : rejectedPlan;
}

/**
 * Returns a sub-element of the 'cachedPlan' explain output which represents a query plan.
 */
export function getCachedPlan(cachedPlan) {
    // The 'queryPlan' format is used when the SBE engine is turned on. If this field is present, it
    // will hold a serialized cached plan, otherwise it will be stored in the 'cachedPlan' field
    // itself.
    return cachedPlan.hasOwnProperty("queryPlan") ? cachedPlan.queryPlan : cachedPlan;
}

function isPlainObject(value) {
    return value && typeof (value) == "object" && value.constructor === Object;
}

/**
 * Flattens the given plan by turning it into an array of stages/children. It excludes fields which
 * might differ in the explain across multiple executions of the same query.
 */
export function flattenPlan(plan) {
    const results = [];

    if (!isPlainObject(plan)) {
        return results;
    }

    const childFields = [
        "inputStage",
        "inputStages",
        "thenStage",
        "elseStage",
        "outerStage",
        "stages",
        "innerStage",
        "child",
        "leftChild",
        "rightChild"
    ];

    // Expand this array if you find new fields which are inconsistent across different test runs.
    const ignoreFields = ["isCached", "indexVersion", "planNodeId"];

    // Iterates over the plan while ignoring the `ignoreFields`, to create flattened stages whenever
    // `childFields` are encountered.
    const stack = [["root", {...plan}]];
    while (stack.length > 0) {
        const [_, next] = stack.pop();
        ignoreFields.forEach(field => delete next[field]);

        for (const childField of childFields) {
            if (childField in next) {
                const child = next[childField];
                delete next[childField];
                if (Array.isArray(child)) {
                    for (let i = 0; i < child.length; i++) {
                        stack.push([childField, child[i]]);
                    }
                } else {
                    stack.push([childField, child]);
                }
            }
        }

        results.push(next);
    }

    return results;
}

/**
 * Returns an object containing the winning plan and an array of rejected plans for the given
 * queryPlanner. Each of those plans is returned in its flattened form.
 */
export function formatQueryPlanner(queryPlanner) {
    return {
        winningPlan: flattenPlan(getWinningPlanFromExplain(queryPlanner)),
        rejectedPlans: queryPlanner.rejectedPlans.map(flattenPlan),
    };
}

/**
 * Formats the given pipeline, which must be an array of stage objects. Returns an array of
 * formatted stages. It excludes fields which might differ in the explain across multiple executions
 * of the same query.
 */
export function formatPipeline(pipeline) {
    const results = [];

    // Pipeline must be an array of objects
    if (!pipeline || !Array.isArray(pipeline) || !pipeline.every(isPlainObject)) {
        return results;
    }

    // Expand this array if you find new fields which are inconsistent across different test runs.
    const ignoreFields = ["lsid"];

    for (const stage of pipeline) {
        const keys = Object.keys(stage).filter(key => key.startsWith("$"));
        if (keys.length !== 1) {
            throw Error("This is not a stage: " + tojson(stage));
        }

        const stageName = keys[0];
        if (stageName == "$cursor") {
            const queryPlanner = stage[stageName].queryPlanner;
            results.push({[stageName]: formatQueryPlanner(queryPlanner)});
        } else {
            const stageCopy = {...stage[stageName]};
            ignoreFields.forEach(field => delete stageCopy[field]);
            // Don't keep any fields that are on the same level as the stage name
            results.push({[stageName]: stageCopy});
        }
    }

    return results;
}

/**
 * Helper function to only add `field` to `dest` if it is present in `src`. A lambda can be passed
 * to transform the field value when it is added to `dest`.
 */
function addIfPresent(field, src, dest, lambda = i => i) {
    if (src && dest && field in src) {
        dest[field] = lambda(src[field]);
    }
}

/**
 * If queryPlanner contains an array of shards, this returns both the merger part and shards
 * part. Both are flattened.
 */
function invertShards(queryPlanner) {
    const winningPlan = queryPlanner.winningPlan;
    const shards = winningPlan.shards;
    if (!Array.isArray(shards)) {
        throw Error("Expected shards field to be array, got: " + tojson(shards));
    }

    const topStage = {...winningPlan};
    delete topStage.shards;

    const res = {mergerPart: flattenPlan(topStage), shardsPart: {}};
    shards.forEach(shard => res.shardsPart[shard.shardName] = formatQueryPlanner(shard));

    return res;
}

/**
 * Returns a formatted version of the explain, excluding fields which might differ in the explain
 * across multiple executions of the same query (e.g. caching information or UUIDs).
 */
export function formatExplainRoot(explain) {
    let res = {};
    if (!isPlainObject(explain)) {
        return res;
    }

    addIfPresent("mergeType", explain, res);
    if ("splitPipeline" in explain) {
        addIfPresent("mergerPart", explain.splitPipeline, res, formatPipeline);
        addIfPresent("shardsPart", explain.splitPipeline, res, formatPipeline);
    }

    if ("shards" in explain) {
        for (const [shardName, shardExplain] of Object.entries(explain["shards"])) {
            res[shardName] = ("queryPlanner" in shardExplain)
                ? formatQueryPlanner(shardExplain.queryPlanner)
                : formatPipeline(shardExplain.stages);
        }
    } else if ("queryPlanner" in explain && "shards" in explain.queryPlanner.winningPlan) {
        res = {...res, ...invertShards(explain.queryPlanner)};
    } else if ("queryPlanner" in explain) {
        res = {...res, ...formatQueryPlanner(explain.queryPlanner)};
    } else if ("stages" in explain) {
        res.stages = formatPipeline(explain.stages);
    }

    return res;
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns all
 * subdocuments whose stage is 'stage'. Returns an empty array if the plan does not have the
 * requested stage. if 'stage' is 'null' returns all the stages in 'root'.
 */
export function getPlanStages(root, stage) {
    var results = [];

    if (root.stage === stage || stage === undefined || root.nodeType === stage) {
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
        results =
            results.concat(getPlanStages(getWinningPlanFromExplain(root.queryPlanner), stage));
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

    if ("queryPlan" in root) {
        results = results.concat(getPlanStages(root.queryPlan, stage));
    }

    if ("child" in root) {
        results = results.concat(getPlanStages(root.child, stage));
    }

    if ("leftChild" in root) {
        results = results.concat(getPlanStages(root.leftChild, stage));
    }

    if ("rightChild" in root) {
        results = results.concat(getPlanStages(root.rightChild, stage));
    }

    if ("shards" in root) {
        if (Array.isArray(root.shards)) {
            results = root.shards.reduce(
                (res, shard) => res.concat(getPlanStages(shard.hasOwnProperty("winningPlan")
                                                             ? getWinningPlanFromExplain(shard)
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
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns a list of
 * all the stages in 'root'.
 */
export function getAllPlanStages(root) {
    return getPlanStages(root);
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns the
 * subdocument with its stage as 'stage'. Returns null if the plan does not have such a stage.
 * Asserts that no more than one stage is a match.
 */
export function getPlanStage(root, stage) {
    assert(stage, "Stage was not defined in getPlanStage.");
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
export function getRejectedPlans(root) {
    if (root.hasOwnProperty('queryPlanner')) {
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
    } else {
        return root.stages[0]['$cursor'].queryPlanner.rejectedPlans;
    }
}

/**
 * Given the root stage of explain's JSON representation of a query plan ('root'), returns true if
 * the query planner reports at least one rejected alternative plan, and false otherwise.
 */
export function hasRejectedPlans(root) {
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
export function getExecutionStages(root) {
    if (root.hasOwnProperty("executionStats") &&
        root.executionStats.executionStages.hasOwnProperty("shards")) {
        const executionStages = [];
        for (let shard of root.executionStats.executionStages.shards) {
            executionStages.push(Object.assign(
                {shardName: shard.shardName, executionSuccess: shard.executionSuccess},
                shard.executionStages));
        }
        return executionStages;
    }
    if (root.hasOwnProperty("shards")) {
        const executionStages = [];
        for (const shard in root.shards) {
            executionStages.push(root.shards[shard].executionStats.executionStages);
        }
        return executionStages;
    }
    return [root.executionStats.executionStages];
}

/**
 * Returns an array of "executionStats" from the given replset or sharded explain output.
 */
export function getExecutionStats(root) {
    if (root.hasOwnProperty("shards")) {
        return Object.values(root.shards).map(shardExplain => shardExplain.executionStats);
    }
    assert(root.hasOwnProperty("executionStats"), root);
    if (root.executionStats.hasOwnProperty("executionStages") &&
        root.executionStats.executionStages.hasOwnProperty("shards")) {
        return root.executionStats.executionStages.shards;
    }
    return [root.executionStats];
}

/**
 * Returns the winningPlan.queryPlan of each shard in the explain in a list.
 */
export function getShardQueryPlans(root) {
    let result = [];

    if (root.hasOwnProperty("shards")) {
        for (let shardName of Object.keys(root.shards)) {
            let shard = root.shards[shardName];
            result.push(shard.queryPlanner.winningPlan.queryPlan);
        }
    } else {
        for (let shard of root.queryPlanner.winningPlan.shards) {
            result.push(shard.winningPlan.queryPlan);
        }
    }

    return result;
}

/**
 * Returns an array of strings representing the "planSummary" values found in the input explain.
 * Assumes the given input is the root of an explain.
 *
 * The helper supports sharded and unsharded explain.
 */
export function getPlanSummaries(root) {
    let res = [];

    // Queries that use the find system have top-level queryPlanner and winningPlan fields. If it's
    // a sharded query, the shards have their own winningPlan fields to look at.
    if ("queryPlanner" in root && "winningPlan" in root.queryPlanner) {
        const wp = root.queryPlanner.winningPlan;

        if ("shards" in wp) {
            for (let shard of wp.shards) {
                res.push(shard.winningPlan.planSummary);
            }
        } else {
            res.push(wp.planSummary);
        }
    }

    // Queries that use the agg system either have a top-level stages field or a top-level shards
    // field. getQueryPlanner pulls the queryPlanner out of the stages/cursor subfields.
    if ("stages" in root) {
        res.push(getQueryPlanner(root).winningPlan.planSummary);
    }

    if ("shards" in root) {
        for (let shardName of Object.keys(root.shards)) {
            let shard = root.shards[shardName];
            res.push(getQueryPlanner(shard).winningPlan.planSummary);
        }
    }

    return res.filter(elem => elem !== undefined);
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
export function getAggPlanStages(root, stage, useQueryPlannerSection = false) {
    assert(stage, "Stage was not defined in getAggPlanStages.");
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
            results = results.concat(
                getPlanStages(getWinningPlanFromExplain(queryLayerOutput.queryPlanner), stage));
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
export function getAggPlanStage(root, stage, useQueryPlannerSection = false) {
    assert(stage, "Stage was not defined in getAggPlanStage.");
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
 * the $unionWith stage of the plan.
 *
 * The normal getAggPlanStages() doesn't find the $unionWith stage in the sharded scenario since it
 * exists in the splitPipeline.
 **/
export function getUnionWithStage(root) {
    if (root.splitPipeline != null) {
        // If there is only one shard, the whole pipeline will run on that shard.
        const subAggPipe = root.splitPipeline === null ? root.shards["shard-rs0"].stages
                                                       : root.splitPipeline.mergerPart;
        for (let i = 0; i < subAggPipe.length; i++) {
            const stage = subAggPipe[i];
            if (stage.hasOwnProperty("$unionWith")) {
                return stage;
            }
        }
    } else {
        return getAggPlanStage(root, "$unionWith");
    }
}

/**
 * Given the root stage of agg explain's JSON representation of a query plan ('root'), returns
 * whether the plan has a stage called 'stage'. It could have more than one to allow for sharded
 * explain plans, and it can search for a query planner stage like "FETCH" or an agg stage like
 * "$group."
 */
export function aggPlanHasStage(root, stage) {
    return getAggPlanStages(root, stage).length > 0;
}

/**
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan has a stage called 'stage'.
 */
export function planHasStage(db, root, stage) {
    assert(stage, "Stage was not defined in planHasStage.");
    return getPlanStages(root, stage).length > 0;
}

/**
 * A query is covered iff it does *not* have a FETCH stage or a COLLSCAN.
 *
 * Given the root stage of explain's BSON representation of a query plan ('root'),
 * returns true if the plan is index only. Otherwise returns false.
 */
export function isIndexOnly(db, root) {
    return !planHasStage(db, root, "FETCH") && !planHasStage(db, root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * an index scan, and false otherwise.
 */
export function isIxscan(db, root) {
    return planHasStage(db, root, "IXSCAN");
}

/**
 * Returns true if the plan is formed of a single EOF stage. False otherwise.
 */
export function isEofPlan(db, root) {
    return planHasStage(db, root, "EOF");
}

/**
 * Returns true if the plan contains fetch stages containing '$alwaysFalse' filters, or false
 * otherwise.
 */
export function isAlwaysFalsePlan(root) {
    const hasAlwaysFalseFilter = (stage) =>
        stage && stage.filter && stage.filter["$alwaysFalse"] === 1;
    return getPlanStages(root, "FETCH").every(hasAlwaysFalseFilter);
}

export function isIdhackOrExpress(db, root) {
    return isExpress(db, root) || isIdhack(db, root);
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the idhack fast path, and false otherwise. These can be represented either as
 * explicit 'IDHACK' or as 'CLUSTERED_IXSCAN' stages with equal min & max
 * record bounds in the case of clustered collections.
 */
export function isIdhack(db, root) {
    if (planHasStage(db, root, "IDHACK")) {
        return true;
    }
    if (!isClusteredIxscan(db, root)) {
        return false;
    }
    const stage = getPlanStages(root, "CLUSTERED_IXSCAN")[0];
    if (stage.minRecord instanceof ObjectId) {
        return stage.minRecord.equals(stage.maxRecord);
    } else {
        if (isObject(stage.minRecord) && isObject(stage.maxRecord)) {
            return documentEq(stage.minRecord, stage.maxRecord);
        }
        return stage.minRecord === stage.maxRecord;
    }
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * the EXPRESS executor, and false otherwise.
 */
export function isExpress(db, root) {
    return planHasStage(db, root, "EXPRESS_IXSCAN") ||
        planHasStage(db, root, "EXPRESS_CLUSTERED_IXSCAN") ||
        planHasStage(db, root, "EXPRESS_UPDATE") || planHasStage(db, root, "EXPRESS_DELETE");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a collection scan, and false otherwise.
 */
export function isCollscan(db, root) {
    return planHasStage(db, root, "COLLSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using
 * a clustered Ix scan, and false otherwise.
 */
export function isClusteredIxscan(db, root) {
    return planHasStage(db, root, "CLUSTERED_IXSCAN");
}

/**
 * Returns true if the BSON representation of a plan rooted at 'root' is using the aggregation
 * framework, and false otherwise.
 */
export function isAggregationPlan(root) {
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
export function isQueryPlan(root) {
    if (root.hasOwnProperty("shards")) {
        const shards = Object.keys(root.shards);
        return shards.reduce(
                   (res, shard) => res + root.shards[shard].hasOwnProperty("queryPlanner") ? 1 : 0,
                   0) > 0;
    }
    return root.hasOwnProperty("queryPlanner");
}

/**
 *  Returns true if every winning plan present in the explain satisfies the predicate. Returns
 *  false otherwise.
 */
export function everyWinningPlan(explain, predicate) {
    return getQueryPlanners(explain)
        .map(queryPlanner => getWinningPlanFromExplain(queryPlanner, false))
        .every(predicate);
}

/**
 * Get the "chunk skips" for a single shard. Here, "chunk skips" refer to documents excluded by the
 * shard filter.
 */
export function getChunkSkipsFromShard(shardPlan, shardExecutionStages) {
    const shardFilterPlanStage =
        getPlanStage(getWinningPlanFromExplain(shardPlan), "SHARDING_FILTER");
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
export function getChunkSkipsFromAllShards(explainResult) {
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
 * Given explain output at executionStats level verbosity, for a count query, confirms that the root
 * stage is COUNT or RECORD_STORE_FAST_COUNT and that the result of the count is equal to
 * 'expectedCount'.
 */
export function assertExplainCount({explainResults, expectedCount}) {
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
export function assertCoveredQueryAndCount({collection, query, project, count}) {
    let explain = collection.find(query, project).explain();
    assert(isIndexOnly(db, getWinningPlanFromExplain(explain.queryPlanner)),
           "Winning plan was not covered: " + tojson(explain.queryPlanner.winningPlan));

    // Same query as a count command should also be covered.
    explain = collection.explain("executionStats").find(query).count();
    assert(isIndexOnly(db, getWinningPlanFromExplain(explain.queryPlanner)),
           "Winning plan for count was not covered: " + tojson(explain.queryPlanner.winningPlan));
    assertExplainCount({explainResults: explain, expectedCount: count});
}

/**
 * Runs explain() operation on 'cmdObj' and verifies that all the stages in 'expectedStages' are
 * present exactly once in the plan returned. When 'stagesNotExpected' array is passed, also
 * verifies that none of those stages are present in the explain() plan.
 */
export function assertStagesForExplainOfCommand({coll, cmdObj, expectedStages, stagesNotExpected}) {
    const plan = assert.commandWorked(coll.runCommand({explain: cmdObj}));
    const winningPlan = getWinningPlanFromExplain(plan.queryPlanner);
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
 * Get the 'planCacheKey' from 'explain'.
 */
export function getPlanCacheKeyFromExplain(explain) {
    return getQueryPlanners(explain)
        .map(qp => {
            assert(qp.hasOwnProperty("planCacheKey"));
            return qp.planCacheKey;
        })
        .at(0);
}

/**
 * Get the 'planCacheShapeHash' from 'object'.
 */
export function getPlanCacheShapeHashFromObject(object) {
    // TODO SERVER 93305: Remove deprecated 'queryHash' usages.
    const planCacheShapeHash = object.planCacheShapeHash || object.queryHash;
    assert.neq(planCacheShapeHash, undefined);
    return planCacheShapeHash;
}

/**
 * Get the 'planCacheShapeHash' from 'explain'.
 */
export function getPlanCacheShapeHashFromExplain(explain) {
    return getQueryPlanners(explain).map(getPlanCacheShapeHashFromObject).reduce((hash0, hash1) => {
        assert.eq(hash0, hash1);
        return hash0;
    });
}

/**
 * Helper to run a explain on the given query shape and get the "planCacheKey" from the explain
 * result.
 */
export function getPlanCacheKeyFromShape(
    {query = {}, projection = {}, sort = {}, collation = {}, collection, db}) {
    const explainRes = assert.commandWorked(
        collection.explain().find(query, projection).collation(collation).sort(sort).finish());

    return getPlanCacheKeyFromExplain(explainRes);
}

/**
 * Helper to run a explain on the given pipeline and get the "planCacheKey" from the explain
 * result.
 */
export function getPlanCacheKeyFromPipeline(pipeline, collection) {
    const explainRes = assert.commandWorked(collection.explain().aggregate(pipeline));
    return getPlanCacheKeyFromExplain(explainRes);
}

/**
 * Given the winning query plan, flatten query plan tree into a list of plan stage names.
 */
export function flattenQueryPlanTree(winningPlan) {
    let stages = [];
    while (winningPlan) {
        stages.push(winningPlan.stage);
        winningPlan = winningPlan.inputStage;
    }
    stages.reverse();
    return stages;
}

/**
 * Assert that a command plan has no FETCH stage or if the stage is present, it has no filter.
 */
export function assertNoFetchFilter({coll, cmdObj}) {
    const plan = assert.commandWorked(coll.runCommand({explain: cmdObj}));
    const winningPlan = getWinningPlanFromExplain(plan.queryPlanner);
    const fetch = getPlanStage(winningPlan, "FETCH");
    assert((fetch === null || !fetch.hasOwnProperty("filter")),
           "Unexpected fetch: " + tojson(fetch));
    return winningPlan;
}

/**
 * Assert that a find plan has a FETCH stage with expected filter and returns a specified number of
 * results.
 */
export function assertFetchFilter({coll, predicate, expectedFilter, nReturned}) {
    const exp = coll.find(predicate).explain("executionStats");
    const plan = getWinningPlanFromExplain(exp.queryPlanner);
    const fetch = getPlanStage(plan, "FETCH");
    assert(fetch !== null, "Missing FETCH stage " + plan);
    assert(fetch.hasOwnProperty("filter"),
           "Expected filter in the fetch stage, got " + tojson(fetch));
    assert.eq(expectedFilter,
              fetch.filter,
              "Expected filter " + tojson(expectedFilter) + " got " + tojson(fetch.filter));

    if (nReturned !== null) {
        assert.eq(exp.executionStats.nReturned,
                  nReturned,
                  "Expected " + nReturned + " documents, got " + exp.executionStats.nReturned);
    }
}

/**
 * Recursively checks if a javascript object contains a nested property key and returns the values.
 */
export function getNestedProperties(object, key) {
    let accumulator = [];

    function traverse(object) {
        if (typeof object !== "object") {
            return;
        }

        for (const k in object) {
            if (k == key) {
                accumulator.push(object[k]);
            }

            traverse(object[k]);
        }
        return;
    }

    traverse(object);
    return accumulator;
}

/**
 * Recognizes the query engine used by the query (sbe/classic).
 */
export function getEngine(explain) {
    const sbePlans = getQueryPlanners(explain).flatMap(
        queryPlanner => getNestedProperties(queryPlanner, "slotBasedPlan"));
    return sbePlans.length == 0 ? "classic" : "sbe";
}

/**
 * Asserts that a pipeline runs with the engine that is passed in as a parameter.
 */
export function assertEngine(pipeline, engine, coll) {
    const explain = coll.explain().aggregate(pipeline);
    assert.eq(getEngine(explain), engine);
}

/**
 * Returns the number of index scans in a query plan.
 */
export function getNumberOfIndexScans(explain) {
    const indexScans = getPlanStages(getWinningPlanFromExplain(explain.queryPlanner), "IXSCAN");
    return indexScans.length;
}

/**
 * Returns the number of column scans in a query plan.
 */
export function getNumberOfColumnScans(explain) {
    const columnIndexScans =
        getPlanStages(getWinningPlanFromExplain(explain.queryPlanner), "COLUMN_SCAN");
    return columnIndexScans.length;
}

/*
 * Returns whether a query is using a multikey index.
 */
export function isIxscanMultikey(winningPlan) {
    let ixscanStage = getPlanStage(winningPlan, "IXSCAN");
    return ixscanStage && ixscanStage.isMultiKey;
}

/**
 * Verify that the explain command output 'explain' shows a BATCHED_DELETE stage with an
 * nWouldDelete value equal to 'nWouldDelete'.
 */
export function checkNWouldDelete(explain, nWouldDelete) {
    assert.commandWorked(explain);
    assert("executionStats" in explain);
    var executionStats = explain.executionStats;
    assert("executionStages" in executionStats);

    // If passed through mongos, then BATCHED_DELETE stage(s) should be below the SHARD_WRITE
    // mongos stage.  Otherwise the BATCHED_DELETE stage is the root stage.
    var execStages = executionStats.executionStages;
    if ("SHARD_WRITE" === execStages.stage) {
        let totalToBeDeletedAcrossAllShards = 0;
        execStages.shards.forEach(function(shardExplain) {
            const rootStageName = shardExplain.executionStages.stage;
            assert(rootStageName === "BATCHED_DELETE", tojson(execStages));
            totalToBeDeletedAcrossAllShards += shardExplain.executionStages.nWouldDelete;
        });
        assert.eq(totalToBeDeletedAcrossAllShards, nWouldDelete, explain);
    } else {
        assert(execStages.stage === "BATCHED_DELETE", explain);
        assert.eq(execStages.nWouldDelete, nWouldDelete, explain);
    }
}

/**
 * Recursively remove fields which conditionally appear in plans that may contribute to spurious
 * differences. Modifies the parameter in-place, no return value.
 */
export function canonicalizePlan(p) {
    delete p.planNodeId;
    delete p.isCached;
    delete p.cardinalityEstimate;
    delete p.costEstimate;
    delete p.estimatesMetadata;
    if (p.hasOwnProperty("inputStage")) {
        canonicalizePlan(p.inputStage);
    } else if (p.hasOwnProperty("inputStages")) {
        p.inputStages.forEach((s) => {
            canonicalizePlan(s);
        });
    }
}

/**
 * Returns index of stage in a aggregation pipeline stage plan running on a single node
 * (will not work for sharded clusters).
 * 'root' is root of explain JSON.
 * Returns -1 if stage does not exist.
 */
export function getIndexOfStageOnSingleNode(root, stageName) {
    if (root.hasOwnProperty("stages")) {
        for (let i = 0; i < root.stages.length; i++) {
            if (root.stages[i].hasOwnProperty(stageName)) {
                return i;
            }
        }
    }
    return -1;
}
