/**
 * Test that confirms all cursors have been cleaned when a shard has failed with an error in the
 * isNotPrimaryError category
 *
 * @tags: [
 * requires_fcv_71
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = 'test';
const collName = 'foo';
const ns = dbName + '.' + collName;

const st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 2}});

let coll = st.s.getDB(dbName)[collName];
coll.insert({x: 1});
coll.insert({x: 2});
coll.insert({x: 3});
coll.insert({x: 4});
coll.insert({x: 5});

// Establish cursor in a multi-doc transaction.
let session = st.s.startSession();
let sessionColl = session.getDatabase(dbName)[collName];
session.startTransaction();

let cursor = sessionColl.find().batchSize(2).comment('myCursor');

cursor.next();
cursor.next();

// Stepdown shard primary.
let shardPrimary = st.rs0.getPrimary();
shardPrimary.adminCommand({replSetStepDown: 60, force: true});

// Continue iterating cursor, which will require issuing a getMore. It will fail because the node
// where we established the cursor is no longer primary.
assert.throws(() => {
    cursor.next();
});

// Kill the cursor (on mongos).
cursor.close();

// Check that the shard cursors eventually gets killed.
assert.soon(() => {
    return shardPrimary.getDB('admin')
               .aggregate([
                   {$currentOp: {idleCursors: true, allUsers: true}},
                   {$match: {type: 'idleCursor', 'cursor.originatingCommand.comment': 'myCursor'}}
               ])
               .itcount() === 0;
});

st.stop();