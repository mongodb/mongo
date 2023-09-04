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

for (let i = 0; i < 1000; ++i) {
    docs.push({a: 1, b: "hello", c: i * 12, d: 111 * i - 100, h: i});
    docs.push({a: i + 1000, b: `hello%{i}`, c: i * 77, d: -i, h: i});
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
        assert.docEq(indexScans[i]['keyPattern'],
                     expectedIndexKeyPatterns[i],
                     JSON.stringify(explain, undefined, 2));
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
        assert.docEq(indexScans[i]['keyPattern'], expectedIndexKeyPatterns[i]);
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

function preferShortestIndexWithComparisonsInFilter() {
    const indexes = [{a: 1, b: 1, c: 1}, {a: 1, b: 1}];
    const filter = {a: {$gt: 1}, b: "hello"};
    assert.commandWorked(coll.createIndexes(indexes));

    assertIndexScan(false, filter, [{a: 1, b: 1, c: 1}]);
    assertIndexScan(true, filter, [{b: 1, a: 1}]);

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
    const filter = {a: 10, b: {$in: [5, 6]}, c: {$gt: 3}};
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

// Running tests.

preferLongestIndexPrefix();
preferEquality();
preferShortestIndex();
preferShortestIndexWithComparisonsInFilter();
notBrokenTie();
multiIntervalIndexBounds();
nonBlockingSort();
blockingSort();
multiIndexScan();
preferLongestPrefixWithIndexesOfSameLength();

// Test finalization.
MongoRunner.stopMongod(conn);
