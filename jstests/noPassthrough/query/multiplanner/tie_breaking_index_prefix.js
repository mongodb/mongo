/**
 * Tests index prefix tie breaking heuritic for multiplanner.
 */

"use strict";

import {getPlanStages, getWinningPlanFromExplain, getEngine} from "jstests/libs/query/analyze_plan.js";
import {getPlanRankerMode, isPlanCosted} from "jstests/libs/query/cbr_utils.js";

// Test initialization.

const options = {};
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up with options: " + tojson(options));
const db = conn.getDB("tie_breaking_index_prefix");

const coll = db.index_prefix;
coll.drop();

const docs = [];

let smallObjectID = undefined;
let largeObjectID = undefined;

for (let i = 0; i < 5000; ++i) {
    const oid = ObjectId();
    if (i === 0) {
        smallObjectID = oid;
    } else if (i === 1000) {
        largeObjectID = oid;
    }

    docs.push({
        a: 1,
        b: "hello",
        c: i * 12,
        d: 111 * i - 100,
        h: i,
        long: NumberLong(i),
        double: 1.0 * i,
        decimal: NumberDecimal(i),
        date: new Date(i),
        timestamp: Timestamp(i, 0),
        string: `abc${1e9 + i}`,
        object: {"a": i},
        objectid: oid,
        array: [i],
    });
    docs.push({
        a: i + 1000,
        b: `hello%{i}`,
        c: i * 77,
        d: -i,
        h: i,
        long: NumberLong(i),
        double: 1.0 * i,
        decimal: NumberDecimal(i),
        date: new Date(i),
        timestamp: Timestamp(i, 0),
        string: `abc${1e9 + i}`,
        object: {"a": i},
        objectid: oid,
        array: [i],
    });
}

assert.commandWorked(coll.insertMany(docs));

function setParamsAndRunCommand(isTieBreakingHeuristicEnabled, filter) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryPlanTieBreakingWithIndexHeuristics: isTieBreakingHeuristicEnabled,
        }),
    );

    const explain = assert.commandWorked(coll.find(filter).explain(true));
    return explain;
}

function assertIndexScan(isTieBreakingHeuristicEnabled, filter, expectedIndexKeyPatterns, explain = null) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryPlanTieBreakingWithIndexHeuristics: isTieBreakingHeuristicEnabled,
        }),
    );

    if (explain === null) {
        explain = assert.commandWorked(coll.find(filter).explain(true));
    }

    const indexScans = getPlanStages(explain, "IXSCAN");

    assert.eq(expectedIndexKeyPatterns.length, indexScans.length);

    for (let i = 0; i < expectedIndexKeyPatterns.length; ++i) {
        assert.eq(indexScans[i]["keyPattern"], expectedIndexKeyPatterns[i], tojson(explain));
    }
}

function assertIndexScanWithSort(isTieBreakingHeuristicEnabled, filter, sorting, expectedIndexKeyPatterns) {
    assert.commandWorked(
        db.adminCommand({
            setParameter: 1,
            internalQueryPlanTieBreakingWithIndexHeuristics: isTieBreakingHeuristicEnabled,
        }),
    );

    const explain = assert.commandWorked(coll.find(filter).sort(sorting).explain(true));

    const indexScans = getPlanStages(explain, "IXSCAN");

    assert.eq(expectedIndexKeyPatterns.length, indexScans.length);

    for (let i = 0; i < expectedIndexKeyPatterns.length; ++i) {
        assert.eq(indexScans[i]["keyPattern"], expectedIndexKeyPatterns[i]);
    }
}

// Test definitions.

function preferLongestIndexPrefix() {
    const indexes = [{a: 1}, {b: 1, a: 1}];
    const filter = {a: 1, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferEquality() {
    const indexes = [
        {a: 1, b: 1},
        {b: 1, a: 1},
    ];
    const filter = {a: {$gt: 0}, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferClosedIntervalsForType(fieldName, typeSmallValue, typeLargeValue) {
    const indexes = [
        {[fieldName]: 1, b: 1},
        {b: 1, [fieldName]: 1},
    ];
    const filterGT = {[fieldName]: {$gt: typeSmallValue}, b: {$gte: "hello", $lt: "hello0"}};
    const filterLT = {[fieldName]: {$lt: typeLargeValue}, b: {$gte: "hello", $lt: "hello0"}};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filterGT, [{[fieldName]: 1, b: 1}]);
    assertIndexScan(true, filterGT, [{b: 1, [fieldName]: 1}]);

    assertIndexScan(false, filterLT, [{[fieldName]: 1, b: 1}]);
    assertIndexScan(true, filterLT, [{b: 1, [fieldName]: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferShortestIndex() {
    const indexes = [
        {a: 1, b: 1, c: 1},
        {b: 1, a: 1},
    ];
    const filter = {a: 1, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1, c: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferShortestIndexWithComparisonsInFilter(indexPruningActive) {
    const indexes = [
        {a: 1, b: 1, c: 1},
        {a: 1, b: 1},
    ];
    const filter = {a: {$gt: 1}, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    // Index pruning would have removed the a/b/c index for us already, so a/b would win.
    if (indexPruningActive) {
        assertIndexScan(false, filter, [{a: 1, b: 1}]);
    } else {
        const explain = setParamsAndRunCommand(false, filter);
        const winningPlan = getWinningPlanFromExplain(explain);

        // If we fall back to CBR, we will choose the smaller index regardless of whether index pruning is used or not.
        if (isPlanCosted(winningPlan)) {
            assertIndexScan(false, filter, [{a: 1, b: 1}], explain);
        } else {
            assertIndexScan(false, filter, [{a: 1, b: 1, c: 1}], explain);
        }
    }
    assertIndexScan(true, filter, [{a: 1, b: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// A tie which is not broken keep the order of the plans the same.
function notBrokenTie() {
    const indexes = [
        {a: 1, b: 1},
        {b: 1, a: 1},
    ];
    const filter = {a: 1, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{a: 1, b: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function multiIntervalIndexBounds() {
    const indexes = [
        {a: 1, b: 1},
        {a: 1, b: 1, c: 1},
    ];
    const filter = {a: 1, b: {$gte: "hello"}, c: {$gte: 0}};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{a: 1, b: 1, c: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function nonBlockingSort() {
    const indexes = [
        {a: 1, b: 1},
        {a: 1, b: 1, c: 1},
    ];
    const filter = {a: 10, b: {$in: [5, 6]}, c: {$gt: 3}};
    const sorting = {a: -1};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScanWithSort(false, filter, sorting, [{a: 1, b: 1}]);
    assertIndexScanWithSort(true, filter, sorting, [{a: 1, b: 1, c: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function blockingSort() {
    const indexes = [
        {a: 1, b: 1},
        {a: 1, b: 1, c: 1},
    ];
    const filter = {a: 10, b: {$in: [5, 6]}, c: {$gt: 3}};
    const sorting = {d: -1};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScanWithSort(false, filter, sorting, [{a: 1, b: 1}]);
    assertIndexScanWithSort(true, filter, sorting, [{a: 1, b: 1, c: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function multiIndexScan() {
    const indexes = [{a: 1, b: 1}, {a: 1, b: 1, c: 1}, {d: 1}];
    const filter = {$or: [{a: 10, b: {$in: [5, 6]}, c: {$gt: 3}}, {d: 1}]};
    assert.commandWorked(coll.createIndexes(indexes));

    // Test with tie breaking heuristics disabled
    assertIndexScan(false, filter, [{a: 1, b: 1}, {d: 1}]);

    // Test with tie breaking heuristics enabled
    // There are 3 cases now handled by this test:

    // SBE (delayOrSkipSubplanning() returns false if the engine is SBE, so essentially we are running in multiPlanning
    // mode here regardless of whether automaticCE is enabled or not)
    // - Subplanner runs
    // - Chooses plan with {a, b, c} index
    // - 0 rejected plans

    // Classic engine in multiPlanning mode
    // - Subplanner runs
    // - Chooses plan with {a, b, c} index (same plan as when using SBE)
    // - 0 rejected plans

    // Classic engine in automaticCE mode
    // - Subplanner does not run, planner does not fall back to CBR. Multiplanner chooses.
    // - Chooses plan with {a, b} index
    // - Does not fall back to CBR, subplanner does not run
    // - 1 rejected plan (this is the plan chosen in multiPlanning mode/when running on SBE)

    // When the multiplanner runs, the tie breaking heuristics are identical between the two plans because they are being
    // calculated for the plan as a whole. When the subplanner runs, these calculations are done only for the relevant
    // index scans and so the longer index {a, b, c} wins.
    const explain = setParamsAndRunCommand(true, filter);
    if (getEngine(explain) == "classic" && getPlanRankerMode(db) == "automaticCE") {
        // When automaticCE ranking mode is enabled and the classic engine is used, even if we don't fall back to CBR we do not
        // call the subplanner in this case. This yields a different result.
        assertIndexScan(true, filter, [{a: 1, b: 1}, {d: 1}], explain);
    } else {
        // If the SBE engine (which will behave as if ranking mode is multiPlanning) is used or plan ranking mode is set to
        // multiPlanning, the subplanner will run.
        assertIndexScan(true, filter, [{a: 1, b: 1, c: 1}, {d: 1}], explain);
    }

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// SERVER-13211
function preferLongestPrefixWithIndexesOfSameLength() {
    const indexes = [
        {a: 1, b: 1},
        {a: 1, c: 1},
        {a: 1, d: 1},
        {a: 1, h: 1},
    ];
    const filter = {a: 1, h: 1};

    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(true, filter, [{a: 1, h: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// Running tests, with index pruning disabled and then enabled.
function testWithPruningSetting(indexPruningActive) {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerEnableIndexPruning: indexPruningActive}),
    );

    preferLongestIndexPrefix();
    preferEquality();
    preferClosedIntervalsForType("long", NumberLong(0), NumberLong(1000));
    preferClosedIntervalsForType("double", 0.0, 1000.0);
    preferClosedIntervalsForType("decimal", NumberDecimal(0), NumberDecimal(1000));
    preferClosedIntervalsForType("date", new Date(0), new Date(1000));
    preferClosedIntervalsForType("timestamp", Timestamp(0, 0), Timestamp(1000, 0));
    preferClosedIntervalsForType("string", `abc${1e9}`, `abc${1e9 + 1000}`);
    preferClosedIntervalsForType("object", {"a": 0}, {"a": 1000});
    preferClosedIntervalsForType("objectid", smallObjectID, largeObjectID);
    preferClosedIntervalsForType("array", [], [1000]);
    preferShortestIndex();
    preferShortestIndexWithComparisonsInFilter(indexPruningActive);
    notBrokenTie();
    multiIntervalIndexBounds();
    nonBlockingSort();
    blockingSort();
    multiIndexScan();
    preferLongestPrefixWithIndexesOfSameLength();
}

testWithPruningSetting(false);
testWithPruningSetting(true);

// Test finalization.
MongoRunner.stopMongod(conn);
