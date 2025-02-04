/*
 * Tests whether a plan summary is returned in the $currentOp output for a find command
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_read_concern_unchanged,
 *   not_allowed_with_signed_security_token,
 *   assumes_unsharded_collection,
 *   requires_getmore,
 *   # The test does not expect concurrent reads against its test collections (e.g. the checks
 *   # aren't expecting concurrent reads but initial sync will be reading those collections).
 *   does_not_support_concurrent_reads,
 * ]
 */

"use strict";

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";

const testDB = db.getSiblingDB("currentop_find");

function assertPlanSummary(collection, expected) {
    const ns = `${testDB.getName()}.${collection.getName()}`;

    const match = {type: "idleCursor", ns};

    const currOpResult = assert.commandWorked(db.adminCommand({
        aggregate: 1,
        cursor: {},
        pipeline: [{$currentOp: {idleCursors: true}}, {$match: match}]
    }));

    const batch = currOpResult.cursor.firstBatch;
    assert.eq(1, batch.length, `Expected only one operation in batch: \n${tojson(batch)}`);
    assert.eq(batch[0].planSummary,
              expected,
              `Response included incorrect planSummary:\n${tojson(batch)}`);
}

const coll = assertDropAndRecreateCollection(testDB, "coll");
{
    const docs = [];
    for (let i = 0; i < 100; ++i) {
        docs.push({"x": i, "y": i + 1});
    }
    assert.commandWorked(coll.insertMany(docs));
}

const coll2 = assertDropAndRecreateCollection(testDB, "coll2");
{
    const docs = [];
    for (let i = 0; i < 1000; ++i) {
        docs.push({"x": i, "y": i + 1});
    }
    docs.push({"x": 1001, "y": -1});
    docs.push({"x": -1, "y": 1001});

    for (let i = 1000; i < 2000; ++i) {
        docs.push({"x": i, "y": [i - 1, i, i + 1]});
    }
    docs.push({x: 1001, y: [1, 2, 3]});
    docs.push({x: 1001, y: [1, 2, 3]});
    assert.commandWorked(coll2.insertMany(docs));
}

// Tests that a collection scan query reports a COLLSCAN plan summary.
{
    const curs = coll.find().batchSize(2);
    curs.next();

    assertPlanSummary(coll, "COLLSCAN");

    curs.close();
}

// Tests that a index scan query reports an IXSCAN plan summary with the correct index.
if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on the cursor_hints_to_query_settings
    // suite. The suite replaces cursor hints with query settings. Query settings do not force
    // indexes, and therefore empty filter will result in collection scans.
    assert.commandWorked(coll.createIndex({x: 1}));
    const curs = coll.find().hint({x: 1}).batchSize(2);
    curs.next();

    assertPlanSummary(coll, "IXSCAN { x: 1 }");

    curs.close();
    assert.commandWorked(coll.dropIndexes());
}

// Tests that an index intersection plan shows both indexes in the plan summary. Also tests
// compound indexes, dotted paths, and an index w/ descending order.
{
    assert.commandWorked(coll2.createIndexes([{x: 1}, {y: 1, 'a.b.c': -1}]));
    const curs = coll2.find({$or: [{x: 1001}, {y: 1001}]}).batchSize(2);
    curs.next();

    assertPlanSummary(coll2, "IXSCAN { x: 1 }, IXSCAN { y: 1, a.b.c: -1 }");

    curs.close();
    assert.commandWorked(coll2.dropIndexes());
}

// Tests that an index scan query returns the correct planSummary when PathTraverse nodes are
// involved (i.e. when one field in the collection has arrays).
{
    assert.commandWorked(coll2.createIndex({y: 1}));
    const curs = coll2.find({y: [1, 2, 3]}).hint({y: 1}).batchSize(2);
    curs.next();

    assertPlanSummary(coll2, "IXSCAN { y: 1 }");

    curs.close();
    assert.commandWorked(coll2.dropIndexes());
}
