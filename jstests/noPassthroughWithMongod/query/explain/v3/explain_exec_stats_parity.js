/**
 * The executionStats section produced by the V3 "execStats" verbosity is information-identical to
 * the one produced by the legacy "executionStats" verbosity for the same command on the same data -
 * recursively identical field sets (a field appearing or disappearing is a failure in either
 * direction) and equal values for every deterministic field.
 * Only inherently run-varying values may differ; they are enumerated in SKIP_FIELDS below.
 * Keep that list short and named: resisting additions to it is what gives this test its teeth. A
 * future "V3-ification" of the retained section must fail here instead of shipping silently.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();
const coll = db[collName];

// The single, named normalization list: timing and yield-dependent counters vary from run to run;
// everything else must be equal.
const SKIP_FIELDS = new Set([
    "executionTimeMillis",
    "executionTimeMillisEstimate",
    "executionTimeMicros",
    "executionTimeNanos",
    "saveState",
    "restoreState",
    "needYield",
]);

// Recursively compares the two executionStats subdocuments: identical key sets at every level,
// and equal values everywhere except the normalized fields.
function assertParity(legacy, v3, path) {
    if (Array.isArray(legacy) || Array.isArray(v3)) {
        assert(Array.isArray(legacy) && Array.isArray(v3), `array/non-array mismatch at ${path}`, {
            legacy,
            v3,
        });
        assert.eq(legacy.length, v3.length, `array length differs at ${path}`, {legacy, v3});
        for (let i = 0; i < legacy.length; ++i) {
            assertParity(legacy[i], v3[i], `${path}[${i}]`);
        }
        return;
    }
    if (typeof legacy === "object" && legacy !== null && typeof v3 === "object" && v3 !== null) {
        const legacyKeys = Object.keys(legacy).sort();
        const v3Keys = Object.keys(v3).sort();
        assert.eq(legacyKeys, v3Keys, `field sets differ at ${path}`, {legacy, v3});
        for (const key of legacyKeys) {
            if (SKIP_FIELDS.has(key)) {
                continue;
            }
            assertParity(legacy[key], v3[key], `${path}.${key}`);
        }
        return;
    }
    assert.eq(legacy, v3, `value differs at ${path}`, {legacy, v3});
}

function assertExecStatsParity(explainedCommand) {
    const legacy = assert.commandWorked(
        db.runCommand({explain: explainedCommand, verbosity: "executionStats"}),
    );
    const v3 = assert.commandWorked(
        db.runCommand({explain: explainedCommand, verbosity: "execStats"}),
    );
    assert.eq(v3.explainVersion, "3", "V3 mode must report explainVersion 3", {v3});
    assert(legacy.executionStats, "missing legacy executionStats", {legacy});
    assert(v3.executionStats, "missing V3 executionStats", {v3});
    // The designed difference vs the legacy *allPlansExecution* verbosity is the absence of the
    // allPlansExecution array; vs the legacy executionStats verbosity compared here, the sections
    // must be information-identical, so the comparison is strict in both directions.
    assertParity(legacy.executionStats, v3.executionStats, "executionStats");
}

describe("V3 execStats executionStats section parity with legacy executionStats", function () {
    let savedFrameworkControl;
    let savedYieldIterations;
    let savedYieldPeriodMS;

    before(function () {
        savedFrameworkControl = assert.commandWorked(
            db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1}),
        ).internalQueryFrameworkControl;
        // Pin the classic engine; SERVER-132033 extends the parity run to SBE.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );

        // The test compares two separate invocations of the same query, so both must execute
        // identically. Count-based yields do (they fire at the same work counts), but time-based
        // yields fire nondeterministically on slow builds (sanitizers, debug) and drift
        // yield-driven counters that are NOT normalized below, e.g. "seeks" (a restore re-seeks
        // the index cursor) and "works" (a NEED_YIELD return adds a work cycle). Disable both
        // yield triggers for the duration of the test.
        const savedYieldParams = assert.commandWorked(
            db.adminCommand({
                getParameter: 1,
                internalQueryExecYieldIterations: 1,
                internalQueryExecYieldPeriodMS: 1,
            }),
        );
        savedYieldIterations = savedYieldParams.internalQueryExecYieldIterations;
        savedYieldPeriodMS = savedYieldParams.internalQueryExecYieldPeriodMS;
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: 1000000000,
                internalQueryExecYieldPeriodMS: 1000000000,
            }),
        );

        coll.drop();
        const docs = [];
        for (let i = 0; i < 500; ++i) {
            docs.push({_id: i, a: i % 100, b: i % 10});
        }
        assert.commandWorked(coll.insert(docs));
        assert.commandWorked(coll.createIndex({a: 1}));
        assert.commandWorked(coll.createIndex({b: 1}));
    });

    after(function () {
        assert.commandWorked(
            db.adminCommand({
                setParameter: 1,
                internalQueryFrameworkControl: savedFrameworkControl,
                internalQueryExecYieldIterations: savedYieldIterations,
                internalQueryExecYieldPeriodMS: savedYieldPeriodMS,
            }),
        );
    });

    it("multi-planned find", function () {
        assertExecStatsParity({find: collName, filter: {a: {$gte: 0}, b: {$gte: 0}}});
    });

    it("single-plan find", function () {
        assertExecStatsParity({find: collName, filter: {nonexistent: 1}});
    });

    it("count", function () {
        assertExecStatsParity({count: collName, query: {a: {$gte: 50}}});
    });

    it("update", function () {
        assertExecStatsParity({
            update: collName,
            updates: [{q: {a: {$gte: 0}, b: {$gte: 0}}, u: {$set: {c: 1}}, multi: true}],
        });
    });
});
