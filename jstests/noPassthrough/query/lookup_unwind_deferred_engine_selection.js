/**
 * Tests expected LU pushdown behavior for different engine knob values.
 *
 * @tags: [
 *     requires_fcv_90
 * ]
 */

import {getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeCompletelyDisabled, checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

function startMongod(flags) {
    return MongoRunner.runMongod({
        setParameter: flags,
    });
}

{
    // Exit early if forceClassic, sbeFull, or trySbe will overwrite our parameters.
    const conn = startMongod({});
    const db = conn.getDB(jsTestName());
    if (checkSbeCompletelyDisabled(db) || checkSbeFullyEnabled(db)) {
        jsTest.log("Exiting early since SBE is fully disabled or fully enabled");
        MongoRunner.stopMongod(conn);
        quit();
    }
    MongoRunner.stopMongod(conn);
}

function getLookupUnwindEngine(conn) {
    const db = conn.getDB(jsTestName());

    const localColl = db.localColl;
    assert(localColl.drop());
    assert.commandWorked(localColl.insert({a: 1}));

    const foreignColl = db.foreignColl;
    assert(foreignColl.drop());
    assert.commandWorked(foreignColl.insert({a: 1}));

    const explain = localColl
        .explain()
        .aggregate([
            {$lookup: {from: "foreignColl", as: "res", localField: "a", foreignField: "a"}},
            {$unwind: "$res"},
        ]);
    return getEngine(explain);
}

function assertLuUsage(flags, expectedEngine) {
    const conn = startMongod(flags);
    try {
        assert.eq(getLookupUnwindEngine(conn), expectedEngine, {flags, expectedEngine});
    } finally {
        MongoRunner.stopMongod(conn);
    }
}

assertLuUsage(
    {
        featureFlagGetExecutorDeferredEngineChoice: true,
        featureFlagSbeEqLookupUnwindHashJoin: true,
        featureFlagSbeEqLookupUnwindLocalCollscan: true,
    },
    "sbe",
);
// We require the deferred get executor to be on for LU to be used.
assertLuUsage({featureFlagGetExecutorDeferredEngineChoice: false}, "classic");
// forceClassic should force LU to use classic, even with the deferred get executor on.
assertLuUsage(
    {
        featureFlagGetExecutorDeferredEngineChoice: true,
        internalQueryFrameworkControl: "forceClassicEngine",
    },
    "classic",
);
// SBE full and trySBE should force LU to use SBE, even with the deferred get executor off.
assertLuUsage({featureFlagGetExecutorDeferredEngineChoice: false, featureFlagSbeFull: true}, "sbe");
assertLuUsage(
    {
        featureFlagGetExecutorDeferredEngineChoice: false,
        internalQueryFrameworkControl: "trySbeEngine",
    },
    "sbe",
);
