/**
 * Tests how replication handles bulkWrite commands with "Timestamp(0,0)" values.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "empty_ts_repl_bulk_write";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
});
rst.startSet();
rst.initiate();

const primaryDb = rst.getPrimary();
const primaryColl = rst.getPrimary().getDB(dbName).getCollection(collName);
const secondaryColl = rst.getSecondary().getDB(dbName).getCollection(collName);
const emptyTs = Timestamp(0, 0);

// Insert several documents. For the first document inserted (_id=201), the empty timestamp value
// in field "a" should get replaced with the current timestamp.
assert.commandWorked(primaryDb.adminCommand({
    bulkWrite: 1,
    ops: [
        {insert: 0, document: {_id: 201, a: emptyTs}},
        {insert: 0, document: {_id: 202, a: 1}},
        {insert: 0, document: {_id: 204, a: 2}},
        {insert: 0, document: {_id: 205, a: 3}},
        {insert: 0, document: {_id: 206, a: 4}}
    ],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Wait for all the inserted documents to replicate to the secondaries.
rst.awaitReplication();

assert.commandWorked(primaryDb.adminCommand({
    bulkWrite: 1,
    ops: [
        // Use a replacement-style update to update _id=202. This should result in field "a" being
        // set to the current timestamp.
        {update: 0, filter: {_id: 202}, updateMods: {a: emptyTs}},

        // Do a replacement-style update to add a new document with _id=203. This should result in
        // field "a" being set to the current timestamp.
        {update: 0, filter: {_id: 203}, updateMods: {a: emptyTs}, upsert: true},

        // For the rest of the commands below, the empty timestamp values stored in field "a" should
        // be preserved as-is.

        // Do an update-operator-style update to update _id=204.
        {update: 0, filter: {_id: 204}, updateMods: {$set: {a: emptyTs}}},

        // Do a pipeline-style update to update _id=205.
        {update: 0, filter: {_id: 205}, updateMods: [{$addFields: {a: emptyTs}}]},

        // Do a pipeline-style update with $internalApplyOplogUpdate to update _id=206.
        {
            update: 0,
            filter: {_id: 206},
            updateMods:
                [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]
        },

        // Do an update-operator-style update to add a new document with _id=207.
        {update: 0, filter: {_id: 207}, updateMods: {$set: {a: emptyTs}}, upsert: true},

        // Do a pipeline-style update to add a new document with _id=208.
        {update: 0, filter: {_id: 208}, updateMods: [{$addFields: {a: emptyTs}}], upsert: true},

        // Do a pipeline-style update with $internalApplyOplogUpdate to add a new document _id=209.
        {
            update: 0,
            filter: {_id: 209},
            updateMods:
                [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
            upsert: true
        },
    ],
    nsInfo: [{ns: dbName + "." + collName}]
}));

rst.awaitReplication();

// Verify that all the bulkWrite commands behaved the way we expect and that they all were
// replicated correctly to the secondaries.
for (let i = 201; i <= 209; ++i) {
    let result = primaryColl.findOne({_id: i});
    let secondaryResult = secondaryColl.findOne({_id: i});

    assert.eq(tojson(result), tojson(secondaryResult), "_id=" + i);

    if (i >= 204) {
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    } else {
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    }
}

// Insert a document with _id=Timestamp(0,0).
assert.commandWorked(primaryDb.adminCommand({
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: emptyTs, a: 5}}],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Verify the document we just inserted can be retrieved using the filter "{_id: Timestamp(0,0)}".
let result = primaryColl.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.eq(tojson(result.a), tojson(5), "_id=" + tojson(emptyTs));

// Do a replacement-style update on the document.
assert.commandWorked(primaryDb.adminCommand({
    bulkWrite: 1,
    ops: [{update: 0, filter: {_id: emptyTs}, updateMods: {_id: emptyTs, a: emptyTs}}],
    nsInfo: [{ns: dbName + "." + collName}]
}));

// Verify the document we just updated can still be retrieved using "{_id: Timestamp(0,0)}" and
// verify that field "a" was set to the current timestamp.
result = primaryColl.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + tojson(emptyTs));

rst.awaitReplication();

// Verify that the document was replicated correctly to the secondaries.
let secondaryResult = secondaryColl.findOne({_id: emptyTs});
assert.eq(tojson(result), tojson(secondaryResult), "_id=" + tojson(emptyTs));

rst.stopSet();
