/**
 * Tests index prefix tie breaking heuritic for multiplanner.
 * @tags: [
 *   cqf_incompatible,
 * ]
 */

"use strict";

import {getPlanStages} from "jstests/libs/analyze_plan.js";

// Test initialization.

const options = {};
const conn = MongoRunner.runMongod();
assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));
const db = conn.getDB('tie_breaking_index_prefix');

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
        object: {'a': i},
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
        object: {'a': i},
        objectid: oid,
        array: [i],
    });
}

assert.commandWorked(coll.insertMany(docs));

function assertIndexScan(isTieBreakingHeuristicEnabled, filter, expectedIndexKeyPatterns) {
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryPlanTieBreakingWithIndexHeuristics: isTieBreakingHeuristicEnabled
    }));

    const explain = assert.commandWorked(coll.find(filter).explain(true));

    const indexScans = getPlanStages(explain, "IXSCAN");

    assert.eq(expectedIndexKeyPatterns.length, indexScans.length);

    for (let i = 0; i < expectedIndexKeyPatterns.length; ++i) {
        assert.eq(indexScans[i]['keyPattern'], expectedIndexKeyPatterns[i]);
    }
}

function assertIndexScanWithSort(
    isTieBreakingHeuristicEnabled, filter, sorting, expectedIndexKeyPatterns) {
    assert.commandWorked(db.adminCommand({
        setParameter: 1,
        internalQueryPlanTieBreakingWithIndexHeuristics: isTieBreakingHeuristicEnabled
    }));

    const explain = assert.commandWorked(coll.find(filter).sort(sorting).explain(true));

    const indexScans = getPlanStages(explain, "IXSCAN");

    assert.eq(expectedIndexKeyPatterns.length, indexScans.length);

    for (let i = 0; i < expectedIndexKeyPatterns.length; ++i) {
        assert.eq(indexScans[i]['keyPattern'], expectedIndexKeyPatterns[i]);
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
    const indexes = [{a: 1, b: 1}, {b: 1, a: 1}];
    const filter = {a: {$gt: 0}, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferClosedIntervalsForType(fieldName, typeSmallValue, typeLargeValue) {
    const indexes = [{[fieldName]: 1, b: 1}, {b: 1, [fieldName]: 1}];
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
    const indexes = [{a: 1, b: 1, c: 1}, {b: 1, a: 1}];
    const filter = {a: 1, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1, c: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function preferShortestIndexWithComparisonsInFilter(indexPruningActive) {
    const indexes = [{a: 1, b: 1, c: 1}, {a: 1, b: 1}];
    const filter = {a: {$gt: 1}, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    // Index pruning would have removed the a/b/c index for us already, so a/b would win.
    if (indexPruningActive) {
        assertIndexScan(false, filter, [{a: 1, b: 1}]);
    } else {
        assertIndexScan(false, filter, [{a: 1, b: 1, c: 1}]);
    }
    assertIndexScan(true, filter, [{a: 1, b: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// A tie which is not broken keep the order of the plans the same.
function notBrokenTie() {
    const indexes = [{a: 1, b: 1}, {b: 1, a: 1}];
    const filter = {a: 1, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{a: 1, b: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function multiIntervalIndexBounds() {
    const indexes = [{a: 1, b: 1}, {a: 1, b: 1, c: 1}];
    const filter = {a: 1, b: {$gte: "hello"}, c: {$gte: 0}};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1}]);
    assertIndexScan(true, filter, [{a: 1, b: 1, c: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

function nonBlockingSort() {
    const indexes = [{a: 1, b: 1}, {a: 1, b: 1, c: 1}];
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
    const indexes = [{a: 1, b: 1}, {a: 1, b: 1, c: 1}];
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

    assertIndexScan(false, filter, [{a: 1, b: 1}, {d: 1}]);
    assertIndexScan(true, filter, [{a: 1, b: 1, c: 1}, {d: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// SERVER-13211
function preferLongestPrefixWithIndexesOfSameLength() {
    const indexes = [{a: 1, b: 1}, {a: 1, c: 1}, {a: 1, d: 1}, {a: 1, h: 1}];
    const filter = {a: 1, h: 1};

    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(true, filter, [{a: 1, h: 1}]);

    for (const index of indexes) {
        assert.commandWorked(coll.dropIndex(index));
    }
}

// Running tests, with index pruning disabled and then enabled.
function testWithPruningSetting(indexPruningActive) {
    assert.commandWorked(db.adminCommand(
        {setParameter: 1, internalQueryPlannerEnableIndexPruning: indexPruningActive}));

    preferLongestIndexPrefix();
    preferEquality();
    preferClosedIntervalsForType('long', NumberLong(0), NumberLong(1000));
    preferClosedIntervalsForType('double', 0.0, 1000.0);
    preferClosedIntervalsForType('decimal', NumberDecimal(0), NumberDecimal(1000));
    preferClosedIntervalsForType('date', new Date(0), new Date(1000));
    preferClosedIntervalsForType('timestamp', Timestamp(0, 0), Timestamp(1000, 0));
    preferClosedIntervalsForType('string', `abc${1e9}`, `abc${1e9 + 1000}`);
    preferClosedIntervalsForType('object', {'a': 0}, {'a': 1000});
    preferClosedIntervalsForType('objectid', smallObjectID, largeObjectID);
    preferClosedIntervalsForType('array', [], [1000]);
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
