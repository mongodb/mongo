/**
 * Tests the explain support for the COLUMN_SCAN stage.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");         // For planHasStage.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
load("jstests/libs/sbe_explain_helpers.js");  // For getSbePlanStages.

const columnstoreEnabled =
    checkSBEEnabled(db, ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"]);
if (!columnstoreEnabled) {
    jsTestLog("Skipping columnstore index validation test since the feature flag is not enabled.");
    return;
}

const coll = db.column_scan_explain;
coll.drop();

assert.commandWorked(coll.createIndex({"$**": "columnstore"}));
const docs = [
    {_id: 0, x: 1, y: [{a: 2}, {a: 3}, {a: 4}]},
    {_id: 1, x: 1},
    {_id: 2, x: 1, y: [{b: 5}, {b: 6}, {b: 7}]},
    {_id: 3, x: 1, y: [{b: 5}, {b: [1, 2, {c: 5}]}, {c: 7}]},
    {_id: 4, x: 1, y: [{b: {c: 1}}]}
];
assert.commandWorked(coll.insertMany(docs));

// Test the explain output for a scan on two columns: one nested and one top-level.
(function testScanOnTwoColumns() {
    const explain = coll.find({}, {x: 1, 'y.a': 1}).explain("executionStats");

    const columnScanStages = getSbePlanStages(explain, "COLUMN_SCAN");
    assert.eq(columnScanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);

    // Verifying column fields.
    const columns = columnScanStages[0].columns;
    assert.eq(Object.keys(columns).length, 4, 'Number of columns should be 4.');
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": 5, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": 5, "numSeeks": 1, "usedInOutput": true},
        "x": {"numNexts": 5, "numSeeks": 1, "usedInOutput": true},
        "y.a": {"numNexts": 1, "numSeeks": 1, "usedInOutput": true}
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column ${columnName}`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScanStages[0].parentColumns;
    assert.eq(Object.keys(parentColumns).length, 1, 'Number of parent columns should be 1.');
    // Expecting 4 lookups on the "y" parent column for the 3 docs which didn't have a "y.a"
    // value and 1 for an unsuccessful call to seek.
    assert.eq(sortDoc(parentColumns.y),
              {"numNexts": 0, "numSeeks": 4},
              'Mismatching entry for parent column \'y\'');

    assert.eq(explain.executionStats.totalKeysExamined, 24, `Mismatch in totalKeysExamined.`);
    assert.eq(
        columnScanStages[0].numRowStoreFetches, 0, 'Number of row store fetches should be 0.');
    assert.eq(columnScanStages[0].nReturned, 5, 'Number returned should be 5.');
}());

// Test the explain output for a scan on a nonexistent field.
(function testNonexistentField() {
    const explain = coll.find({}, {z: 1}).explain("executionStats");

    const columnScanStages = getSbePlanStages(explain, "COLUMN_SCAN");
    assert.eq(columnScanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);

    // Verifying column fields.
    const columns = columnScanStages[0].columns;
    assert.eq(Object.keys(columns).length, 3, 'Number of columns should be 3.');
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": 5, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": 5, "numSeeks": 1, "usedInOutput": true},
        "z": {"numNexts": 0, "numSeeks": 1, "usedInOutput": true},
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column ${columnName}`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScanStages[0].parentColumns;
    assert.eq(parentColumns, {});

    assert.eq(explain.executionStats.totalKeysExamined, 13, `Mismatch in totalKeysExamined.`);
    assert.eq(
        columnScanStages[0].numRowStoreFetches, 0, 'Number of row store fetches should be 0.');
    assert.eq(columnScanStages[0].nReturned, 5, 'Number returned should be 5.');
}());

// Test the explain output for a scan on a 2-level nested field.
(function testMultipleNestedColumns() {
    const explain = coll.find({}, {'y.b.c': 1}).explain("executionStats");

    const columnScanStages = getSbePlanStages(explain, "COLUMN_SCAN");
    assert.eq(columnScanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);

    // Verifying column fields.
    const columns = columnScanStages[0].columns;
    assert.eq(Object.keys(columns).length, 3, 'Number of columns should be 3.');
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": 5, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": 5, "numSeeks": 1, "usedInOutput": true},
        "y.b.c": {"numNexts": 2, "numSeeks": 1, "usedInOutput": true},
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column ${columnName}`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScanStages[0].parentColumns;
    assert.eq(Object.keys(parentColumns).length, 2, 'Number of parent columns should be 2.');
    // Expecting 3 lookups on the "y" parent column for the 2 docs which didn't have a "y.b"
    // value and 1 unsuccessful call to seek.
    assert.eq(sortDoc(parentColumns.y),
              {"numNexts": 0, "numSeeks": 3},
              'Mismatching entry for parent column \'y\'');
    // Expecting 4 lookups on the "y.b" parent column for the 3 docs that didn't have a "y.b.c"
    // value and 1 unsuccessful call to seek.
    assert.eq(sortDoc(parentColumns['y.b']),
              {"numNexts": 0, "numSeeks": 4},
              'Mismatching entry for parent column \'y.b\'');

    assert.eq(explain.executionStats.totalKeysExamined, 22, `Mismatch in totalKeysExamined.`);
    assert.eq(
        columnScanStages[0].numRowStoreFetches, 0, 'Number of row store fetches should be 0.');
    assert.eq(columnScanStages[0].nReturned, 5, 'Number returned should be 5.');
}());
}());
