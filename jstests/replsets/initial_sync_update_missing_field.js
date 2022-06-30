/**
 * Initial sync runs in several phases - the first 3 are as follows:
 * 1) fetches the last oplog entry (op_start1) on the source;
 * 2) copies all non-local databases from the source; and
 * 3) fetches and applies operations from the source after op_start1.
 *
 * Between phases 1 and 2, this test updates array fields and subdocument fields with both the
 * "update" and "applyOps" commands on the source, then replaces the array/subdoc fields with
 * strings. The secondary will fail to apply the update operation in phase 3 but initial sync
 * completes nevertheless. The absence of the array/subdoc on the source indicates that a later
 * operation has replaced the field, so the target is free to ignore the failed update operation.
 */

(function() {
load("jstests/replsets/libs/initial_sync_update_missing_doc.js");

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const dbName = 'test';
const collectionName = jsTestName();

const db = primary.getDB(dbName);
const coll = db.getCollection(collectionName);

jsTestLog("Insert some documents with array and subdocument fields");

for (let i = 0; i < 8; ++i) {
    assert.commandWorked(coll.insertOne({_id: i, array: [0], doc: {field: 0}}));
}

jsTestLog("Add a secondary");

const secondaryConfig = {
    rsConfig: {votes: 0, priority: 0}
};
const secondary = reInitiateSetWithSecondary(replSet, secondaryConfig);

jsTestLog("Use both 'update' and 'applyOps' to update docs on primary");

coll.updateMany({}, {$set: {scalar: 0}});

// Update the 8 documents in different ways:
//     * use updateOne or applyOps
//     * update the subdocument or array field
//     * also update the scalar field, or don't
assert.commandWorked(coll.updateOne({_id: 1}, {$set: {'doc.field': 1, 'scalar': 1}}));
assert.commandWorked(coll.updateOne({_id: 0}, {$set: {'doc.field': 1}}));

assert.commandWorked(coll.updateOne({_id: 3}, {$set: {'array.0': 1, 'scalar': 1}}));
assert.commandWorked(coll.updateOne({_id: 2}, {$set: {'array.0': 1}}));

assert.commandWorked(primary.adminCommand({
    applyOps: [{
        op: 'u',
        ns: coll.getFullName(),
        o2: {_id: 5},
        o: {$v: 2, diff: {u: {'scalar': 1}, sdoc: {u: {field: 1}}}}
    }]
}));

assert.commandWorked(primary.adminCommand({
    applyOps:
        [{op: 'u', ns: coll.getFullName(), o2: {_id: 4}, o: {$v: 2, diff: {sdoc: {u: {field: 1}}}}}]
}));

assert.commandWorked(primary.adminCommand({
    applyOps: [{
        op: 'u',
        ns: coll.getFullName(),
        o2: {_id: 7},
        o: {$v: 2, diff: {u: {'scalar': 1}, sarray: {a: true, u0: 1}}}
    }]
}));

assert.commandWorked(primary.adminCommand({
    applyOps: [{
        op: 'u',
        ns: coll.getFullName(),
        o2: {_id: 6},
        o: {$v: 2, diff: {sarray: {a: true, u0: 1}}}
    }]
}));

jsTestLog("Set array and subdoc fields to strings on primary");

assert.commandWorked(coll.updateMany({}, {$set: {array: 'string', doc: 'string'}}));

jsTestLog("Allow initial sync to finish");

assert.commandWorked(secondary.getDB('admin').runCommand(
    {configureFailPoint: 'initialSyncHangBeforeCopyingDatabases', mode: 'off'}));

jsTestLog(`Collection on primary: ${tojson(coll.find().toArray())}`);

finishAndValidate(replSet, collectionName, 8);

replSet.stopSet();
})();
