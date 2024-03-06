/*
 * Tests whether a plan summary is returned in the $currentOp output for a find command
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   not_allowed_with_signed_security_token,
 *   assumes_unsharded_collection
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
testDB.createCollection("coll");
testDB.createCollection("coll2");

const coll = testDB.coll;
const coll2 = testDB.coll2;

for (let i = 0; i < 100; ++i) {
    coll.insert({"x": i, "y": i + 1});
}

for (let i = 0; i < 1000; ++i) {
    coll2.insert({"x": i, "y": i + 1});
}
coll2.insert({"x": 1001, "y": -1});
coll2.insert({"x": -1, "y": 1001});

for (let i = 1000; i < 2000; ++i) {
    coll2.insert({"x": i, "y": [i - 1, i, i + 1]});
}
coll2.insert({x: 1001, y: [1, 2, 3]});
coll2.insert({x: 1001, y: [1, 2, 3]});
// Tests that a collection scan query reports a COLLSCAN plan summary.
{
    let curs = coll.find().batchSize(2);
    curs.next();
    let currOpResult = db.adminCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}]
    });

    let planSummary = "";
    var batch = currOpResult.cursor.firstBatch;
    for (var i = 0; i < batch.length; i++) {
        if (batch[i].ns == "currentop_find.coll") {
            planSummary = batch[i].planSummary;
            break;
        }
    }
    assert.eq(planSummary, "COLLSCAN", "Response included incorrect or empty planSummary");
    curs.close();
}

// Tests that a index scan query reports an IXSCAN plan summary with the correct index.
if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on the cursor_hints_to_query_settings
    // suite. The suite replaces cursor hints with query settings. Query settings do not force
    // indexes, and therefore empty filter will result in collection scans.
    coll.createIndex({x: 1});
    let curs = coll.find().hint({x: 1}).batchSize(2);
    curs.next();

    let currOpResult = db.adminCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}]
    });

    let planSummary = "";
    var batch = currOpResult.cursor.firstBatch;
    for (var i = 0; i < batch.length; i++) {
        if (batch[i].ns == "currentop_find.coll") {
            planSummary = batch[i].planSummary;
            break;
        }
    }
    assert.eq(planSummary, "IXSCAN { x: 1 }", "Response included incorrect or empty planSummary ");
    curs.close();
}

// // Tests that an index intersection plan shows both indexes in the plan summary. Also tests
// // compound indexes, dotted paths, and an index w/ descending order.
{
    coll2.createIndexes([{x: 1}, {y: 1, 'a.b.c': -1}]);
    let curs = coll2.find({$or: [{x: 1001}, {y: 1001}]}).batchSize(2);
    curs.next();

    let currOpResult = db.adminCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}]
    });
    let planSummary = "";
    var batch = currOpResult.cursor.firstBatch;
    for (var i = 0; i < batch.length; i++) {
        if (batch[i].ns == "currentop_find.coll2") {
            planSummary = batch[i].planSummary;
            break;
        }
    }
    assert.eq(planSummary,
              "IXSCAN { x: 1 }, IXSCAN { y: 1, a.b.c: -1 }",
              "Response included incorrect or empty planSummary");
    curs.close();
    coll2.dropIndex('x_1');
    coll2.dropIndex({y: 1, 'a.b.c': -1});
}

// Tests that an index scan query returns the correct planSummary when PathTraverse nodes are
// involved (i.e. when one field in the collection has arrays).
{
    coll2.createIndex({y: 1});
    let curs = coll2.find({y: [1, 2, 3]}).hint({y: 1}).batchSize(2);
    curs.next();
    let currOpResult = db.adminCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [{$currentOp: {idleCursors: true}}, {$match: {type: "idleCursor"}}]
    });
    let planSummary = "";
    var batch = currOpResult.cursor.firstBatch;

    for (var i = 0; i < batch.length; i++) {
        if (batch[i].ns == "currentop_find.coll2") {
            planSummary = batch[i].planSummary;
            break;
        }
    }
    assert.eq(planSummary, "IXSCAN { y: 1 }", "Response included incorrect or empty planSummary ");
    curs.close();
}
})();
