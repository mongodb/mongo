/**
 * Tests that replacement style update with $v field in the document is correctly applied.
 * @tags: [
 *  requires_fcv_50,
 * ]
 */

(function() {
"use strict";

const st = new ShardingTest({nodes: 2});

const dbName = 'testDb';
const collName = 'testColl';
const coll = st.s.getDB(dbName).getCollection(collName);

st.adminCommand({enablesharding: dbName});
const oplog = st.getPrimaryShard(dbName).getDB('local').getCollection('oplog.rs');

function assertLastUpdateOplogEntryIsReplacement() {
    const lastUpdate = oplog.find({op: 'u'}).sort({$natural: -1}).limit(1).next();
    assert(lastUpdate.o._id);
}

[true].forEach($v => {
    const _id = assert.commandWorked(coll.insertOne({$v})).insertedId;
    assert.commandWorked(coll.update({_id}, [{$set: {p: 1, q: 1}}]));
    assertLastUpdateOplogEntryIsReplacement();
});

[true, "hello", 0, 1, 2, 3].forEach($v => {
    const _id = assert.commandWorked(coll.insertOne({})).insertedId;
    assert.commandWorked(coll.update(
        {_id},
        [{$replaceWith: {"$setField": {field: {$literal: "$v"}, input: "$$ROOT", value: $v}}}]));
    assertLastUpdateOplogEntryIsReplacement();
});

(function() {
const _id = assert.commandWorked(coll.insertOne({})).insertedId;
assert.commandWorked(coll.update(
    {_id},
    [{$replaceWith: {"$setField": {field: {$literal: "$set"}, input: "$$ROOT", value: {a: 1}}}}]));
assertLastUpdateOplogEntryIsReplacement();
})();

st.stop();
}());
