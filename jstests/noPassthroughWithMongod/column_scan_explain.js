/**
 * Tests the explain support for the COLUMN_SCAN stage.
 * @tags: [
 *   # column store indexes are still under a feature flag and require full sbe
 *   uses_column_store_index,
 *   featureFlagColumnstoreIndexes,
 *   featureFlagSbeFull,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq
load("jstests/libs/analyze_plan.js");         // For planHasStage.
load("jstests/libs/sbe_util.js");             // For checkSBEEnabled.
load("jstests/libs/sbe_explain_helpers.js");  // For getSbePlanStages.

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

    // Validate SBE part.
    const columnScanStages = getSbePlanStages(explain, "columnscan");
    assert.eq(columnScanStages.length, 1, `Could not find 'columnscan' stage: ${tojson(explain)}`);
    const columnScan = columnScanStages[0];

    assertArrayEq({
        actual: columnScan.paths,
        expected: ["_id", "x", "y.a"],
        extraErrorMsg: 'Paths used by column scan stage'
    });

    // Verifying column fields.
    const columns = columnScan.columns;
    assert.eq(
        Object.keys(columns).length, 4, `Should access 4 columns but accessed: ${tojson(columns)}`);

    // We seek into each column once, when setting up the cursors. The dense column is the first
    // to hit EOF after iterating over all documents so other columns iterate at least one time
    // less.
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": docs.length, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": docs.length - 1, "numSeeks": 1, "usedInOutput": true},
        "x": {"numNexts": docs.length - 1, "numSeeks": 1, "usedInOutput": true},
        "y.a": {"numNexts": 1, "numSeeks": 1, "usedInOutput": true}
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column ${tojson(columnName)}`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScan.parentColumns;
    assert.eq(Object.keys(parentColumns).length,
              1,
              `Should access 1 parent column but accessed: ${tojson(parentColumns)}`);
    // Expecting 4 lookups on the "y" parent column for the 3 docs which didn't have a "y.a"
    // value and 1 for an unsuccessful call to seek. We should not iterate over parent columns.
    assert.eq(sortDoc(parentColumns.y),
              {"numNexts": 0, "numSeeks": 4},
              'Mismatching entry for parent column "y"');

    // 'totalKeysExamined' should be equal to the sum of "next" and "seek" calls across all
    // columns.
    assert.eq(explain.executionStats.totalKeysExamined,
              columns["<<RowId Column>>"].numNexts + columns["<<RowId Column>>"].numSeeks +
                  columns["_id"].numNexts + columns["_id"].numSeeks + columns["x"].numNexts +
                  columns["x"].numSeeks + columns["y.a"].numNexts + columns["y.a"].numSeeks +
                  parentColumns["y"].numNexts + parentColumns["y"].numSeeks,
              `Mismatch in totalKeysExamined.`);

    assert.eq(columnScan.numRowStoreFetches, 0, 'Mismatch in numRowStoreFetches');
    assert.eq(columnScan.nReturned, docs.length, 'nReturned: should return all docs');

    // Validate QSN part.
    const columnScanPlanStages = getPlanStages(explain, "COLUMN_SCAN");
    assert.eq(
        columnScanPlanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);
    assert(documentEq(columnScanPlanStages[0],
                      {"allFields": ["_id", "x", "y.a"], "extraFieldsPermitted": true},
                      false /* verbose */,
                      null /* valueComparator */,
                      ["stage", "planNodeId"]));
}());

// Test the explain output for a scan on a nonexistent field.
(function testNonexistentField() {
    const explain = coll.find({}, {z: 1}).explain("executionStats");

    // Validate SBE part.
    const columnScanStages = getSbePlanStages(explain, "columnscan");
    assert.eq(columnScanStages.length, 1, `Could not find 'columnscan' stage: ${tojson(explain)}`);
    const columnScan = columnScanStages[0];

    assertArrayEq({
        actual: columnScan.paths,
        expected: ["_id", "z"],
        extraErrorMsg: 'Paths used by column scan stage'
    });

    // Verifying column fields.
    const columns = columnScan.columns;
    assert.eq(
        Object.keys(columns).length, 3, `Should access 3 columns but accessed: ${tojson(columns)}`);
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": docs.length, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": docs.length - 1, "numSeeks": 1, "usedInOutput": true},
        "z": {"numNexts": 0, "numSeeks": 1, "usedInOutput": true},
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column "${columnName}"`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScan.parentColumns;
    assert.eq(parentColumns, {}, "Should not access parent columns");

    // 'totalKeysExamined' should be equal to the sum of "next" and "seek" calls across all
    // columns.
    assert.eq(explain.executionStats.totalKeysExamined,
              columns["<<RowId Column>>"].numNexts + columns["<<RowId Column>>"].numSeeks +
                  columns["_id"].numNexts + columns["_id"].numSeeks + columns["z"].numNexts +
                  columns["z"].numSeeks,
              `Mismatch in totalKeysExamined.`);

    assert.eq(columnScan.numRowStoreFetches, 0, 'Mismatch in numRowStoreFetches');
    assert.eq(columnScan.nReturned, docs.length, 'nReturned: should return all docs');

    // Validate QSN part.
    const columnScanPlanStages = getPlanStages(explain, "COLUMN_SCAN");
    assert.eq(
        columnScanPlanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);
    assert(documentEq(columnScanPlanStages[0],
                      {"allFields": ["_id", "z"], "extraFieldsPermitted": false},
                      false /* verbose */,
                      null /* valueComparator */,
                      ["stage", "planNodeId"]));
}());

// Test the explain output for a scan on a 2-level nested field.
(function testMultipleNestedColumns() {
    const explain = coll.find({}, {'y.b.c': 1}).explain("executionStats");

    // Validate SBE part.
    const columnScanStages = getSbePlanStages(explain, "columnscan");
    assert.eq(columnScanStages.length, 1, `Could not find 'columnscan' stage: ${tojson(explain)}`);
    const columnScan = columnScanStages[0];

    assertArrayEq({
        actual: columnScan.paths,
        expected: ["_id", "y.b.c"],
        extraErrorMsg: 'Paths used by column scan stage'
    });

    // Verifying column fields.
    const columns = columnScan.columns;
    assert.eq(
        Object.keys(columns).length, 3, `Should access 3 columns but accessed: ${tojson(columns)}`);
    const expectedColumns = {
        "<<RowId Column>>": {"numNexts": docs.length, "numSeeks": 1, "usedInOutput": false},
        "_id": {"numNexts": docs.length - 1, "numSeeks": 1, "usedInOutput": true},
        "y.b.c": {"numNexts": 1, "numSeeks": 1, "usedInOutput": true},
    };
    for (const [columnName, expectedObj] of Object.entries(expectedColumns)) {
        assert.eq(sortDoc(columns[columnName]),
                  sortDoc(expectedObj),
                  `Mismatching entry for column ${columnName}`);
    }

    // Verifying parent column fields.
    const parentColumns = columnScan.parentColumns;
    assert.eq(Object.keys(parentColumns).length,
              2,
              `Should access 1 parent column but accessed: ${tojson(parentColumns)}`);
    // Expecting 3 lookups on the "y" parent column for the 2 docs which didn't have a "y.b"
    // value and 1 unsuccessful call to seek. We should not iterate over parent columns.
    assert.eq(sortDoc(parentColumns.y),
              {"numNexts": 0, "numSeeks": 3},
              'Mismatching entry for parent column "y"');
    // Expecting 4 lookups on the "y.b" parent column for the 3 docs that didn't have a "y.b.c"
    // value and 1 unsuccessful call to seek.
    assert.eq(sortDoc(parentColumns['y.b']),
              {"numNexts": 0, "numSeeks": 4},
              'Mismatching entry for parent column "y.b"');

    // 'totalKeysExamined' should be equal to the sum of "next" and "seek" calls across all
    // columns.
    assert.eq(explain.executionStats.totalKeysExamined,
              columns["<<RowId Column>>"].numNexts + columns["<<RowId Column>>"].numSeeks +
                  columns["_id"].numNexts + columns["_id"].numSeeks + columns["y.b.c"].numNexts +
                  columns["y.b.c"].numSeeks + parentColumns["y.b"].numNexts +
                  parentColumns["y.b"].numSeeks + parentColumns["y"].numNexts +
                  parentColumns["y"].numSeeks,
              `Mismatch in totalKeysExamined.`);

    assert.eq(columnScan.numRowStoreFetches, 0, 'Mismatch in numRowStoreFetches');
    assert.eq(columnScan.nReturned, docs.length, 'nReturned: should return all docs');

    // Validate QSN part.
    const columnScanPlanStages = getPlanStages(explain, "COLUMN_SCAN");
    assert.eq(
        columnScanPlanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);
    assert(documentEq(columnScanPlanStages[0],
                      {"allFields": ["_id", "y.b.c"], "extraFieldsPermitted": true},
                      false /* verbose */,
                      null /* valueComparator */,
                      ["stage", "planNodeId"]));
}());

// Test fallback to the row store.
(function testWithFallbackToRowstore() {
    const coll_rowstore = db.column_scan_explain_rowstore;
    coll_rowstore.drop();
    assert.commandWorked(coll_rowstore.createIndex({"$**": "columnstore"}));

    const docs_rowstore = [
        {_id: 0, x: {y: 42, z: 0}},
        {_id: 1, x: {y: {z: 0}}},  // fallback
        {_id: 2, x: [{y: 42}, 0]},
        {_id: 3, x: [{y: 42}, {z: 0}]},
        {_id: 4, x: [{y: [42, 43]}, {z: 0}]},
        {_id: 5, x: [{y: [42, {z: 0}]}, {z: 0}]},  // fallback
        {_id: 6, x: 42},
    ];
    coll_rowstore.insert(docs_rowstore);
    const explain = coll_rowstore.find({}, {_id: 0, "x.y": 1}).explain("executionStats");

    const columnScanStages = getSbePlanStages(explain, "columnscan");
    assert.eq(columnScanStages.length, 1, `Could not find 'columnscan' stage: ${tojson(explain)}`);
    const columnScan = columnScanStages[0];

    assert.eq(columnScan.numRowStoreFetches, 2, 'Mismatch in numRowStoreFetches');
    assert.eq(columnScan.nReturned, docs_rowstore.length, 'nReturned: should return all docs');
}());

// Test the QSN explain output for scans with different types of filters.
(function testScanWithFilters() {
    const explain =
        coll.find({x: 1, 'y.b': 5, $or: [{x: 2}, {'y.a': 3}]}, {_id: 0, x: 1}).explain();

    const columnScanPlanStages = getPlanStages(explain, "COLUMN_SCAN");
    assert.eq(
        columnScanPlanStages.length, 1, `Could not find 'COLUMN_SCAN' stage: ${tojson(explain)}`);
    assert(documentEq(columnScanPlanStages[0],
                      {
                          "allFields": ["x", "y.b", "y.a"],
                          "filtersByPath": {"x": {"$eq": 1}, "y.b": {"$eq": 5}},
                          "residualPredicate": {"$or": [{"x": {"$eq": 2}}, {"y.a": {"$eq": 3}}]},
                          "extraFieldsPermitted": true
                      },
                      false /* verbose */,
                      null /* valueComparator */,
                      ["stage", "planNodeId"]));
}());
}());
