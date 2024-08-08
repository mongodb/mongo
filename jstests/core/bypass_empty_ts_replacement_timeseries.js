/**
 * Tests inserts, updates, and upserts on a timeseries collection with "bypassEmptyTsReplacement"
 * set to true.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_transactions,
 *   requires_fcv_50,
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

const coll = db.jstests_bypass_empty_ts_replacement_timeseries;
const collName = coll.getName();
const emptyTs = Timestamp(0, 0);

coll.drop();

assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}));

function doInsert(docs, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand(
        {insert: collName, documents: docs, bypassEmptyTsReplacement: bypassEmptyTsReplacement});
    assert.commandWorked(cmdRes);
}

function doUpdate(filter, update, bypassEmptyTsReplacement) {
    let cmdRes = db.runCommand({
        update: collName,
        updates: [{q: filter, u: update, multi: true}],
        bypassEmptyTsReplacement: bypassEmptyTsReplacement
    });
    assert.commandWorked(cmdRes);
}

let numCallsToRunTests = 0;

function runTests(bypassEmptyTsReplacement) {
    ++numCallsToRunTests;

    let startId = numCallsToRunTests * 100;

    function getId(i) {
        return startId + i;
    }

    // Insert two documents.
    doInsert([{_id: getId(1), t: new Date(100), m: emptyTs, a: emptyTs}], bypassEmptyTsReplacement);
    doInsert([{_id: getId(2), t: new Date(200), m: getId(2), a: emptyTs}],
             bypassEmptyTsReplacement);

    // Do a replacement-style update containing an empty timestamp value.
    doUpdate({m: getId(2)}, {$set: {m: emptyTs}}, bypassEmptyTsReplacement);

    // Verify that the commands above didn't mutate any of the empty timestamp values in the
    // collection.
    for (let i = 1; i <= 2; ++i) {
        let result = coll.findOne({_id: getId(i)});
        assert.eq(tojson(result.m), tojson(emptyTs), "_id=" + getId(i));
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + getId(i));
    }
}

let bypassEmptyTsReplacement = true;
runTests(bypassEmptyTsReplacement);

bypassEmptyTsReplacement = false;
runTests(bypassEmptyTsReplacement);
}());
