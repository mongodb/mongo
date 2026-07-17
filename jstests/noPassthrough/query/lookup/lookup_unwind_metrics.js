/**
 * Tests that serverStatus metrics under metrics.query.lookupUnwind are incremented correctly
 * when $lookup+$unwind (LU) queries run in SBE. Each counter corresponds to one of the 7
 * LU IFR flags used to incrementally roll out the feature.
 *
 * Requires featureFlagGetExecutorDeferredEngineChoice and SBE to be enabled.
 *
 * @tags: [
 *  # Join reordering may affect metrics expected by some of these test cases.
 *  incompatible_with_join_optimization
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {after, describe, it} from "jstests/libs/mochalite.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTestName());
if (!checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping test because SBE is disabled");
    MongoRunner.stopMongod(conn);
    quit();
}

// featureFlagGetExecutorDeferredEngineChoice gates plan-based engine selection, which is required
// for $lookup-$unwind to run in SBE. disableUnreleasedIFRFlags can disable it, which would cause the
// queries to fall back to classic and prevent the lookupUnwind counters from incrementing.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "GetExecutorDeferredEngineChoice")) {
    jsTest.log.info(
        "Skipping test because featureFlagGetExecutorDeferredEngineChoice is disabled " +
            "(e.g. by disableUnreleasedIFRFlags).",
    );
    MongoRunner.stopMongod(conn);
    quit();
}

// localNoIdx: no index on join field 'a', outer side uses COLLSCAN
// localWithIdx: index on join field 'a', outer side uses IXSCAN+FETCH
// localOrIdx: indexes on 'a' and 'b', outer side uses a complex OR plan
// foreignNoIdx: no index on 'b', triggers NLJ or HJ (with allowDiskUse)
// foreignWithIdx: index on 'b', triggers INLJ
// foreignDinlj: collation-incompatible index on 'b', triggers DINLJ
const localNoIdx = db.localNoIdx;
const localWithIdx = db.localWithIdx;
const localOrIdx = db.localOrIdx;
const foreignNoIdx = db.foreignNoIdx;
const foreignWithIdx = db.foreignWithIdx;
const foreignDinlj = db.foreignDinlj;

for (const coll of [
    localNoIdx,
    localWithIdx,
    localOrIdx,
    foreignNoIdx,
    foreignWithIdx,
    foreignDinlj,
]) {
    coll.drop();
}
for (let i = 0; i < 5; i++) {
    assert.commandWorked(localNoIdx.insert({_id: i, a: i}));
    assert.commandWorked(localWithIdx.insert({_id: i, a: i}));
    assert.commandWorked(localOrIdx.insert({_id: i, a: i, b: i + 1}));
    assert.commandWorked(foreignNoIdx.insert({_id: i, b: i}));
    assert.commandWorked(foreignWithIdx.insert({_id: i, b: i}));
    assert.commandWorked(foreignDinlj.insert({_id: i, b: "hello" + i}));
}
assert.commandWorked(localWithIdx.createIndex({a: 1}));
assert.commandWorked(localOrIdx.createIndex({a: 1}));
assert.commandWorked(localOrIdx.createIndex({b: 1}));
assert.commandWorked(foreignWithIdx.createIndex({b: 1}));
assert.commandWorked(foreignDinlj.createIndex({b: 1}, {collation: {locale: "en", strength: 2}}));

const counterToFeatureFlag = {
    indexedLoopJoin: "SbeEqLookupUnwindIndexedLoopJoin",
    nestedLoopJoin: "SbeEqLookupUnwindNestedLoopJoin",
    hashLookup: "SbeEqLookupUnwindHashJoin",
    dynamicIndexedLoopJoin: "SbeEqLookupUnwindDynamicIndexedLoopJoin",
    localCollscan: "SbeEqLookupUnwindLocalCollscan",
    localIxscanFetch: "SbeEqLookupUnwindLocalIxscanFetch",
    localComplex: "SbeEqLookupUnwindLocalComplexDataAccessPlans",
};

function requiredFlagsEnabled(expectedCounters) {
    return expectedCounters.every((counter) =>
        FeatureFlagUtil.isPresentAndEnabled(db, counterToFeatureFlag[counter]),
    );
}

function getLuCounters() {
    return db.serverStatus().metrics.query.lookupUnwind;
}

function getLookupCounters() {
    return db.serverStatus().metrics.query.lookup;
}

function makeLuPipeline(foreignCollName) {
    return [
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "res"}},
        {$unwind: "$res"},
    ];
}

// A $match on the local join field causes the planner to use IXSCAN+FETCH instead of COLLSCAN.
function makeLuIxscanPipeline(foreignCollName) {
    return [{$match: {a: {$gt: -1}}}, ...makeLuPipeline(foreignCollName)];
}

// An $or on two separately indexed fields triggers a complex OR, FETCH, IXSCAN plan on the outer side.
function makeLuComplexPipeline(foreignCollName) {
    return [
        {$match: {$or: [{a: {$lt: 3}}, {b: {$gt: 2}}]}},
        {$lookup: {from: foreignCollName, localField: "a", foreignField: "b", as: "res"}},
        {$unwind: "$res"},
    ];
}

function assertCounterIncrement(localColl, pipeline, options, expectedCounters) {
    if (!requiredFlagsEnabled(expectedCounters)) {
        jsTest.log.info("Skipping case because a required LU IFR feature flag is disabled", {
            expectedCounters,
        });
        return;
    }

    const beforeLookupUnwind = getLuCounters();
    const beforeLookup = getLookupCounters();
    localColl.aggregate(pipeline, options).toArray();
    const afterLookupUnwind = getLuCounters();
    const afterLookup = getLookupCounters();

    const allCounters = [
        "indexedLoopJoin",
        "nestedLoopJoin",
        "hashLookup",
        "dynamicIndexedLoopJoin",
        "localCollscan",
        "localIxscanFetch",
        "localComplex",
    ];
    for (const counter of allCounters) {
        const delta = afterLookupUnwind[counter] - beforeLookupUnwind[counter];
        const expectedDelta = expectedCounters.includes(counter) ? 1 : 0;
        assert.eq(
            delta,
            expectedDelta,
            `Counter '${counter}' delta was ${delta}, expected ${expectedDelta}`,
        );
    }

    // LU queries must not increment the regular lookup counters.
    for (const c of ["nestedLoopJoin", "indexedLoopJoin", "hashLookup", "dynamicIndexedLoopJoin"]) {
        const delta = afterLookup[c] - beforeLookup[c];
        assert.eq(delta, 0, `Regular lookup counter '${c}' should not increment for LU queries`);
    }
}

describe("lookupUnwind serverStatus metrics", function () {
    it("increments hashLookup + localCollscan for hash join with collscan outer", function () {
        assertCounterIncrement(
            localNoIdx,
            makeLuPipeline(foreignNoIdx.getName()),
            {allowDiskUse: true},
            ["hashLookup", "localCollscan"],
        );
    });

    it("increments indexedLoopJoin + localCollscan for INLJ with collscan outer", function () {
        assertCounterIncrement(localNoIdx, makeLuPipeline(foreignWithIdx.getName()), {}, [
            "indexedLoopJoin",
            "localCollscan",
        ]);
    });

    it("increments nestedLoopJoin + localCollscan for NLJ with collscan outer", function () {
        assertCounterIncrement(
            localNoIdx,
            makeLuPipeline(foreignNoIdx.getName()),
            {allowDiskUse: false},
            ["nestedLoopJoin", "localCollscan"],
        );
    });

    it("increments dynamicIndexedLoopJoin + localCollscan for DINLJ with collscan outer", function () {
        assertCounterIncrement(
            localNoIdx,
            makeLuPipeline(foreignDinlj.getName()),
            {allowDiskUse: false},
            ["dynamicIndexedLoopJoin", "localCollscan"],
        );
    });

    it("increments nestedLoopJoin + localCollscan for non-existent foreign collection", function () {
        assertCounterIncrement(localNoIdx, makeLuPipeline("nonExistentForeignColl"), {}, [
            "nestedLoopJoin",
            "localCollscan",
        ]);
    });

    it("increments hashLookup + localIxscanFetch for hash join with ixscan+fetch outer", function () {
        assertCounterIncrement(
            localWithIdx,
            makeLuIxscanPipeline(foreignNoIdx.getName()),
            {allowDiskUse: true},
            ["hashLookup", "localIxscanFetch"],
        );
    });

    it("increments indexedLoopJoin + localIxscanFetch for INLJ with ixscan+fetch outer", function () {
        assertCounterIncrement(localWithIdx, makeLuIxscanPipeline(foreignWithIdx.getName()), {}, [
            "indexedLoopJoin",
            "localIxscanFetch",
        ]);
    });

    it("increments nestedLoopJoin + localIxscanFetch for NLJ with ixscan+fetch outer", function () {
        assertCounterIncrement(
            localWithIdx,
            makeLuIxscanPipeline(foreignNoIdx.getName()),
            {allowDiskUse: false},
            ["nestedLoopJoin", "localIxscanFetch"],
        );
    });

    it("increments dynamicIndexedLoopJoin + localIxscanFetch for DINLJ with ixscan+fetch outer", function () {
        assertCounterIncrement(
            localWithIdx,
            makeLuIxscanPipeline(foreignDinlj.getName()),
            {allowDiskUse: false},
            ["dynamicIndexedLoopJoin", "localIxscanFetch"],
        );
    });

    it("increments hashLookup + localOther for hash join with complex outer", function () {
        assertCounterIncrement(
            localOrIdx,
            makeLuComplexPipeline(foreignNoIdx.getName()),
            {allowDiskUse: true},
            ["hashLookup", "localComplex"],
        );
    });

    it("increments indexedLoopJoin + localOther for INLJ with complex outer", function () {
        assertCounterIncrement(localOrIdx, makeLuComplexPipeline(foreignWithIdx.getName()), {}, [
            "indexedLoopJoin",
            "localComplex",
        ]);
    });

    it("increments nestedLoopJoin + localOther for NLJ with complex outer", function () {
        assertCounterIncrement(
            localOrIdx,
            makeLuComplexPipeline(foreignNoIdx.getName()),
            {allowDiskUse: false},
            ["nestedLoopJoin", "localComplex"],
        );
    });

    it("increments dynamicIndexedLoopJoin + localOther for DINLJ with complex outer", function () {
        assertCounterIncrement(
            localOrIdx,
            makeLuComplexPipeline(foreignDinlj.getName()),
            {allowDiskUse: false},
            ["dynamicIndexedLoopJoin", "localComplex"],
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });
});
