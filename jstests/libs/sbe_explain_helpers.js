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
 * Helper to make an assertion depending on the engine being used. If we're in a mixed version
 * cluster, then we assert that either 'classicAssert' or 'sbeAssert' is true because the outcome
 * will depend on which node we're making assertions against. If we're not in a mixed version
 * scenario, then we make an assertion depending on the value of 'isSBEEnabled'.
 */
function engineSpecificAssertion(classicAssert, sbeAssert, theDB, msg) {
    if (checkBothEnginesAreRunOnCluster(theDB)) {
        assert(classicAssert || sbeAssert, msg);
    } else if (checkSBEEnabled(theDB)) {
        assert(sbeAssert, msg);
    } else {
        assert(classicAssert, msg);
    }
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

    assert(false, `expected version 2 explain output but got ${explainOutputV2}`);
    return undefined;
}
