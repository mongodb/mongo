/**
 * Test the fast restore procedure for backups, a cursor is open from mongos and then the cursors
 * are consumed in parallel by a direct connection to shards. This way, the cursor already have the
 * filtering information.
 */
// In order for the cursors to be consumed, it is necessary that none of the connections use
// sessions.
TestData.disableImplicitSessions = true;

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let kDbName = 'db';
let kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;
let st = new ShardingTest({shards: 3});
const kShard0Name = st.shard0.shardName;
const kShard1Name = st.shard1.shardName;
const kShard2Name = st.shard2.shardName;
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName, primaryShard: kShard0Name}));
assert.commandWorked(st.s.adminCommand({shardCollection: kNss, key: {x: 1}}));
assert.commandWorked(st.s.getCollection(kNss).insert({x: -10001}));
assert.commandWorked(st.s.getCollection(kNss).insert({x: 0}));
assert.commandWorked(st.s.getCollection(kNss).insert({x: 10001}));
const suspendRangeDeletionShard0Fp =
    configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
// Set up a sharded collection with a chunk and a document on each shard.
assert.commandWorked(st.s.adminCommand({split: kNss, middle: {x: -10000}}));
assert.commandWorked(st.s.adminCommand({split: kNss, middle: {x: 10000}}));
assert.commandWorked(st.s.adminCommand({moveChunk: kNss, find: {x: -10000}, to: kShard1Name}));
suspendRangeDeletionShard0Fp.wait();
assert.commandWorked(st.s.adminCommand({moveChunk: kNss, find: {x: 10000}, to: kShard2Name}));
suspendRangeDeletionShard0Fp.wait();
// Open a cursor with a comment to be consumed on the shard directly.
assert.commandWorked(st.s.getDB(kDbName).runCommand(
    {"find": kCollName, batchSize: 0, sort: {$natural: 1}, comment: kNss}));
let shardCursors = st.s.getDB("admin")
                       .aggregate([
                           {$currentOp: {idleCursors: true}},
                           {$match: {type: "idleCursor", "cursor.originatingCommand.comment": kNss}}
                       ])
                       .map(x => {
                           // By using the comment as an identifier, open a cursor directly on each
                           // shard which should take into consideration ownership of data.
                           let kDbName = x.ns.split(".", 1);
                           let conn = new Mongo(x.host);
                           let cursor = new DBCommandCursor(
                               conn.getDB(kDbName),
                               {ok: 1, cursor: {id: x.cursor.cursorId, ns: x.ns, firstBatch: []}});
                           return cursor;
                       });

assert.eq(1, shardCursors[0].toArray().length);
assert.eq(1, shardCursors[1].toArray().length);
assert.eq(1, shardCursors[2].toArray().length);
suspendRangeDeletionShard0Fp.off();
st.stop();
