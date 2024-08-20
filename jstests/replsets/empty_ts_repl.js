/**
 * Tests how replication handles inserts and updates with "Timestamp(0,0)" values.
 *
 * @tags: [
 *   multiversion_incompatible,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";
const collName = "empty_ts_repl";

const rst = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
});
rst.startSet();
rst.initiate();

const primaryColl = rst.getPrimary().getDB(dbName).getCollection(collName);
const secondaryColl = rst.getSecondary().getDB(dbName).getCollection(collName);
const emptyTs = Timestamp(0, 0);

// Insert several documents. For the first document inserted (_id=101), the empty timestamp value
// in field "a" should get replaced with the current timestamp.
assert.commandWorked(primaryColl.insert({_id: 101, a: emptyTs}));
assert.commandWorked(primaryColl.insert({_id: 102, a: 1}));
assert.commandWorked(primaryColl.insert({_id: 103, a: 2}));
assert.commandWorked(primaryColl.insert({_id: 106, a: 3}));
assert.commandWorked(primaryColl.insert({_id: 107, a: 4}));
assert.commandWorked(primaryColl.insert({_id: 108, a: 5}));
assert.commandWorked(primaryColl.insert({_id: 109, a: 6}));
assert.commandWorked(primaryColl.insert({_id: 110, a: 7}));
assert.commandWorked(primaryColl.insert({_id: 111, a: 8}));

// Wait for all the inserted documents to replicate to the secondaries.
rst.awaitReplication();

// Use a replacement-style update to update _id=102. This should result in field "a" being set to
// the current timestamp.
assert.commandWorked(primaryColl.update({_id: 102}, {a: emptyTs}));

// Use a replacement-style findAndModify to update _id=103. This should result in field "a" being
// set to the current timestamp.
let findAndModifyResult = primaryColl.findAndModify({query: {_id: 103}, update: {a: emptyTs}});
assert.eq(findAndModifyResult, {_id: 103, a: 2});

// Do a replacement-style update to add a new document with _id=104. This should result in field "a"
// being set to the current timestamp.
assert.commandWorked(primaryColl.update({_id: 104}, {a: emptyTs}, {upsert: true}));

// Do a replacement-style findAndModify to add a new document with _id=105. This should result in
// field "a" being set to the current timestamp.
findAndModifyResult =
    primaryColl.findAndModify({query: {_id: 105}, update: {a: emptyTs}, upsert: true});
assert.eq(findAndModifyResult, null);

// For the rest of the commands below, the empty timestamp values stored in field "a" should be
// preserved as-is.

// Do an update-operator-style update to update _id=106.
assert.commandWorked(primaryColl.update({_id: 106}, {$set: {a: emptyTs}}));

// Do an update-operator-style findAndModify to update _id=107.
findAndModifyResult = primaryColl.findAndModify({query: {_id: 107}, update: {$set: {a: emptyTs}}});
assert.eq(findAndModifyResult, {_id: 107, a: 4});

// Do a pipeline-style update to update _id=108.
assert.commandWorked(primaryColl.update({_id: 108}, [{$addFields: {a: emptyTs}}]));

// Do a pipeline-style findAndModify to update _id=109.
findAndModifyResult =
    primaryColl.findAndModify({query: {_id: 109}, update: [{$addFields: {a: emptyTs}}]});
assert.eq(findAndModifyResult, {_id: 109, a: 6});

// Do a pipeline-style update with $internalApplyOplogUpdate to update _id=110.
assert.commandWorked(primaryColl.update(
    {_id: 110}, [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]));

// Do a pipeline-style findAndModify with $internalApplyOplogUpdate to update _id=111.
findAndModifyResult = primaryColl.findAndModify({
    query: {_id: 111},
    update: [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}]
});
assert.eq(findAndModifyResult, {_id: 111, a: 8});

// Do an update-operator-style update to add a new document with _id=112.
assert.commandWorked(primaryColl.update({_id: 112}, {$set: {a: emptyTs}}, {upsert: true}));

// Do an update-operator-style findAndModify to add a new document with _id=113.
findAndModifyResult =
    primaryColl.findAndModify({query: {_id: 113}, update: {$set: {a: emptyTs}}, upsert: true});
assert.eq(findAndModifyResult, null);

// Do a pipeline-style update to add a new document with _id=114.
assert.commandWorked(primaryColl.update({_id: 114}, [{$addFields: {a: emptyTs}}], {upsert: true}));

// Do a pipeline-style findAndModify to add a new document with _id=115.
findAndModifyResult = primaryColl.findAndModify(
    {query: {_id: 115}, update: [{$addFields: {a: emptyTs}}], upsert: true});
assert.eq(findAndModifyResult, null);

// Do a pipeline-style update with $internalApplyOplogUpdate to add a new document _id=116.
assert.commandWorked(primaryColl.update(
    {_id: 116},
    [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
    {upsert: true}));

// Do a pipeline-style findAndModify with $internalApplyOplogUpdate to add a new document _id=117.
findAndModifyResult = primaryColl.findAndModify({
    query: {_id: 117},
    update: [{$_internalApplyOplogUpdate: {oplogUpdate: {$v: 2, diff: {i: {a: emptyTs}}}}}],
    upsert: true
});
assert.eq(findAndModifyResult, null);

rst.awaitReplication();

// Verify that all the insert, update, and findAndModify commands behaved the way we expect and that
// they all were replicated correctly to the secondaries.
for (let i = 101; i <= 117; ++i) {
    let result = primaryColl.findOne({_id: i});
    let secondaryResult = secondaryColl.findOne({_id: i});

    assert.eq(tojson(result), tojson(secondaryResult), "_id=" + i);

    if (i >= 106) {
        assert.eq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    } else {
        assert.neq(tojson(result.a), tojson(emptyTs), "_id=" + i);
    }
}

// Insert a document with _id=Timestamp(0,0).
assert.commandWorked(primaryColl.insert({_id: emptyTs, a: 9}));

// Verify the document we just inserted can be retrieved using the filter "{_id: Timestamp(0,0)}".
let result = primaryColl.findOne({_id: emptyTs});
assert.eq(tojson(result._id), tojson(emptyTs), "_id=" + tojson(emptyTs));
assert.eq(tojson(result.a), tojson(9), "_id=" + tojson(emptyTs));

// Do a replacement-style update on the document.
assert.commandWorked(primaryColl.update({_id: emptyTs}, {_id: emptyTs, a: emptyTs}));

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
