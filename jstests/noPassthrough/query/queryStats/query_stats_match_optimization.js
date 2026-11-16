/**
 * Tests the optimization that pushes a $match on 'keyHash' down into the $queryStats stage, which
 * resolves matching entries through the query stats store's side index instead of scanning the
 * whole store.
 *
 * The optimization is gated by the startup-only parameter
 * 'internalQueryStatsMatchPushdownOptimizationEnabled', so the optimized and unoptimized paths are
 * exercised by two mongods, one per knob value, and every case asserts both return the same
 * results. Every case also checks, via explain, which keyHash targets the pushdown captured. A
 * sharded cluster covers the pushdown when $queryStats runs on mongos, and checks that each mongos
 * resolves only keyHashes recorded in its own store.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getAggPlanStage} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

/// ------------------------------------
/// Helper Functions
/// ------------------------------------

// Runs 'pipeline' against the admin database and returns the sorted keyHashes it produces, if any.
function aggregateQueryStatsKeyHashes(conn, pipeline, batchSize) {
    const adminDB = conn.getDB("admin");
    const res = assert.commandWorked(
        adminDB.runCommand({
            aggregate: 1,
            pipeline: pipeline,
            cursor: batchSize ? {batchSize} : {},
        }),
    );
    return new DBCommandCursor(adminDB, res)
        .toArray()
        .map((e) => e.keyHash)
        .sort();
}

// Returns the sorted keyHash of every entry in the store.
function findAllKeyHashes(conn) {
    return aggregateQueryStatsKeyHashes(conn, [{$queryStats: {}}]);
}

// Returns the sorted keyHashes present in every one of 'conns' stores, so a target taken from here
// is resolvable on all of them. Query stats also records the server's own commands, and those
// recordings can differ between nodes, so the raw stores are not identical.
function commonKeyHashes(conns) {
    const perConn = conns.map((conn) => new Set(findAllKeyHashes(conn)));
    const [first, ...rest] = perConn;
    return [...first].filter((keyHash) => rest.every((hashes) => hashes.has(keyHash))).sort();
}

// Runs $queryStats with a trailing $match on 'matchPred' and returns the sorted keyHashes it
// produces, along with the keyHash targets explain reports the pushdown captured (null when the
// optimization did not fire).
function runQueryStatsWithTrailingMatch(conn, matchPred, batchSize) {
    const pipeline = [{$queryStats: {}}, {$match: matchPred}];
    const keyHashes = aggregateQueryStatsKeyHashes(conn, pipeline, batchSize);

    const explain = assert.commandWorked(
        conn
            .getDB("admin")
            .runCommand({aggregate: 1, pipeline: pipeline, explain: true, cursor: {}}),
    );
    // On mongos, $queryStats runs on the router, so its stages are under 'mongos'.
    const root = explain.mongos ?? explain;
    const queryStatsStage = getAggPlanStage(root, "$queryStats");
    assert(queryStatsStage, "expected a $queryStats stage in EXPLAIN", {explain});
    const lookupLocations = queryStatsStage["$queryStats"].keyHashLookupLocations ?? null;

    return {keyHashes, lookupLocations};
}

// Asserts the $match on keyHash returns 'expectedKeyHashes' and that explain reports
// 'expectedLookupLocations' as the pushed-down targets, or null when the pushdown must not fire.
function assertResultsAndExplainMatchExpected(
    conn,
    matchPred,
    expectedKeyHashes,
    expectedLookupLocations,
    {label, batchSize} = {},
) {
    const {keyHashes, lookupLocations} = runQueryStatsWithTrailingMatch(conn, matchPred, batchSize);
    // A null 'expectedKeyHashes' means the case asserts only the pushdown state: its result set
    // spans the node's whole store, which is not comparable across nodes.
    if (expectedKeyHashes !== null) {
        assert.eq(keyHashes, expectedKeyHashes, "unexpected results", {label, matchPred});
    }
    const sortedOrNull = (arr) => (arr === null ? null : arr.slice().sort());
    assert.eq(
        sortedOrNull(lookupLocations),
        sortedOrNull(expectedLookupLocations),
        "unexpected pushed-down targets",
        {label, matchPred},
    );
}

// Runs 'numShapes' distinct find shapes, so each one gets its own query stats store entry.
function recordDistinctFindShapes(coll, fieldPrefix, numShapes) {
    for (let i = 0; i < numShapes; ++i) {
        coll.find({[fieldPrefix + i]: i}).itcount();
    }
}

// Runs 'numShapes' distinct aggregate shapes, so each one gets its own query stats store entry.
function recordDistinctAggShapes(coll, fieldPrefix, numShapes) {
    for (let i = 0; i < numShapes; ++i) {
        coll.aggregate([{$match: {[fieldPrefix + i]: i}}]).itcount();
    }
}

// Populates the store with 8 distinct find shapes and aggregate shapes.
function populateStore(coll) {
    assert.commandWorked(coll.insert(Array.from({length: 20}, (_, i) => ({a: i, b: i % 3}))));
    recordDistinctFindShapes(coll, "field", 8);
    recordDistinctAggShapes(coll, "field", 8);
}

// Drops the test collection on 'conn' and repopulates it with the standard workload.
function populateColl(conn) {
    const coll = conn.getDB("test")[jsTestName()];
    coll.drop();
    populateStore(coll);
}

// Returns the sorted keyHashes of every entry in 'conn's store, asserting the standard workload was
// recorded.
function snapshotStore(conn) {
    const allKeyHashes = findAllKeyHashes(conn);
    assert.gte(allKeyHashes.length, 16, "expected at least 16 query stats entries", {allKeyHashes});
    return allKeyHashes;
}

// Query stats must record every query, and the store must be sized for the test.
function getQueryStatsServerParams(cacheSize) {
    return {internalQueryStatsSampleRate: 1, internalQueryStatsCacheSize: cacheSize};
}

// Starts a mongod with the keyHash pushdown knob set to 'pushdownEnabled' and populates its store
// with the standard workload. The knob is always set explicitly: leaving it at its default for the
// scan connection would silently collapse the two connections onto the same path if the server
// ended up with the optimization enabled.
function startConn({pushdownEnabled = false} = {}) {
    const setParameter = getQueryStatsServerParams("10MB");
    setParameter.internalQueryStatsMatchPushdownOptimizationEnabled = pushdownEnabled;
    const conn = MongoRunner.runMongod({setParameter: setParameter});
    populateColl(conn);
    return conn;
}

/// ------------------------------------
/// Test Cases
/// ------------------------------------

function defineKeyHashMatchCases(getAllKeyHashes, assertCase) {
    it("resolves an $eq target", function () {
        const target = getAllKeyHashes()[0];
        assertCase({keyHash: target}, [target], [target]);
    });

    it("resolves $in targets", function () {
        const allKeyHashes = getAllKeyHashes();
        const targets = [allKeyHashes[0], allKeyHashes[1], allKeyHashes[2]].sort();
        assertCase({keyHash: {$in: targets}}, targets, targets);
    });

    it("returns nothing for a keyHash that is not in the store", function () {
        const bogus = "0".repeat(64);
        assertCase({keyHash: bogus}, [], [bogus]);
    });

    it("handles an $in mixing present and absent keyHashes", function () {
        const allKeyHashes = getAllKeyHashes();
        const targets = [allKeyHashes[0], "0".repeat(64), allKeyHashes[1]];
        const expected = [allKeyHashes[0], allKeyHashes[1]].sort();
        assertCase({keyHash: {$in: targets}}, expected, targets);
    });

    it("does not push down a $match the index cannot resolve", function () {
        // A regex in $in is not a keyHash target, and '^$' cannot match a non-empty keyHash.
        assertCase({keyHash: {$in: [/^$/]}}, [], null);
    });

    it("returns all matches across getMore batches", function () {
        const allKeyHashes = getAllKeyHashes();
        const targets = allKeyHashes.slice(0, 5).sort();
        assert.gt(targets.length, 1, "need multiple targets to span batches", {allKeyHashes});
        assertCase({keyHash: {$in: targets}}, targets, targets, {batchSize: 1});
    });

    it("pushes down the combined list for an $or of keyHash preds", function () {
        const allKeyHashes = getAllKeyHashes();
        const targets = [allKeyHashes[0], allKeyHashes[1]];
        const expected = targets.slice().sort();
        assertCase({$or: targets.map((keyHash) => ({keyHash}))}, expected, expected);
    });

    it("pushes down an empty list for an $and whose keyHash preds are disjoint", function () {
        const allKeyHashes = getAllKeyHashes();
        assertCase({$and: [{keyHash: allKeyHashes[0]}, {keyHash: allKeyHashes[1]}]}, [], []);
    });

    it("returns each entry once for an $in with duplicate keyHashes", function () {
        // InMatchExpression stores its equalities sorted and deduped, so the duplicate is gone
        // before the rule captures targets; the results and the captured set both have one copy.
        const allKeyHashes = getAllKeyHashes();
        const [first, second] = allKeyHashes;
        const expected = [first, second].sort();
        assertCase({keyHash: {$in: [first, first, second]}}, expected, expected);
    });

    it("does not push down a keyHash under $nor", function () {
        // $nor is not a shape the index can narrow, so no pushdown fires. Its result set is the
        // whole store minus the excluded hash, which depends on each node's own recorded commands,
        // so only the pushdown state is asserted here.
        const excluded = getAllKeyHashes()[0];
        assertCase({$nor: [{keyHash: excluded}]}, /*expectedKeyHashes*/ null, null);
    });
}

describe("running $queryStats followed by $match on mongod: unoptimized vs. optimized paths", function () {
    let connWithPushdown;
    let connWithoutPushdown;

    // Asserts 'expectedKeyHashes' on both the optimized and unoptimized paths match the same
    // expected value. Explain must report 'pushedDownTargets' on the optimized path, null otw.
    // By comparing against the unoptimized path, this test serves as a regression guard.
    function assertOnBothPaths(matchPred, expectedKeyHashes, pushedDownTargets, opts = {}) {
        // Optimized Path
        assertResultsAndExplainMatchExpected(
            connWithPushdown,
            matchPred,
            expectedKeyHashes,
            pushedDownTargets,
            {...opts, label: "pushdown"},
        );
        // Unoptimized Path
        assertResultsAndExplainMatchExpected(
            connWithoutPushdown,
            matchPred,
            expectedKeyHashes,
            null,
            {
                ...opts,
                label: "scan",
            },
        );
    }

    before(function () {
        connWithPushdown = startConn({pushdownEnabled: true});
        connWithoutPushdown = startConn({pushdownEnabled: false});

        // Assert each connection recorded the standard workload. The two stores are deliberately
        // not compared against each other: query stats also records the server's own commands, and
        // those recordings can differ between nodes in either direction.
        snapshotStore(connWithPushdown);
        snapshotStore(connWithoutPushdown);
    });

    after(function () {
        MongoRunner.stopMongod(connWithPushdown);
        MongoRunner.stopMongod(connWithoutPushdown);
    });

    // Targets are taken from the shapes both connections hold, so both paths can resolve them.
    defineKeyHashMatchCases(
        () => commonKeyHashes([connWithPushdown, connWithoutPushdown]),
        assertOnBothPaths,
    );
});

describe("running $queryStats followed by $match on mongos: unoptimized vs. optimized paths", function () {
    let st;
    let otherMongos;
    let conn;

    before(function () {
        st = new ShardingTest({
            shards: 2,
            mongos: 2,
            other: {
                mongosOptions: {
                    setParameter: {
                        ...getQueryStatsServerParams("10MB"),
                        internalQueryStatsMatchPushdownOptimizationEnabled: true,
                    },
                },
            },
        });
        otherMongos = st.s1;
        conn = st.s0;

        populateColl(conn);
        snapshotStore(conn);
    });

    after(function () {
        st.stop();
    });

    defineKeyHashMatchCases(
        () => findAllKeyHashes(conn),
        (matchPred, expectedKeyHashes, expectedTargets, opts) =>
            assertResultsAndExplainMatchExpected(
                conn,
                matchPred,
                expectedKeyHashes,
                expectedTargets,
                opts,
            ),
    );

    // The store is per node, so a keyHash another mongos recorded must resolve to nothing
    // here even though the pushdown still fires locally.
    it("does not resolve a keyHash recorded on another mongos", function () {
        // Snapshot the first mongos's store, then record a shape only on the second.
        const onFirstMongos = new Set(findAllKeyHashes(st.s0));
        otherMongos.getDB("test")[jsTestName()].find({onlyOnTheSecondMongos: 1}).itcount();
        // Pick a keyHash the second mongos recorded and the first did not.
        const target = findAllKeyHashes(otherMongos).find((keyHash) => !onFirstMongos.has(keyHash));
        assert(target, "expected the second mongos to record a shape the first lacks", {
            onFirstMongos,
        });

        // The shape resolves on the mongos that recorded it, but not on the other, even though the
        // pushdown still fires there.
        assertResultsAndExplainMatchExpected(otherMongos, {keyHash: target}, [target], [target], {
            label: "recording mongos",
        });
        assertResultsAndExplainMatchExpected(st.s0, {keyHash: target}, [], [target], {
            label: "other mongos",
        });
    });
});
