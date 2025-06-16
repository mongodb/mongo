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

function setupCluster(st, routerConn, kDbName, kCollName, kNss, cursorComment) {
    const kShard0Name = st.shard0.shardName;
    const kShard1Name = st.shard1.shardName;
    const kShard2Name = st.shard2.shardName;

    assert.commandWorked(
        routerConn.adminCommand({enableSharding: kDbName, primaryShard: kShard0Name}));
    assert.commandWorked(routerConn.adminCommand({shardCollection: kNss, key: {x: 1}}));
    assert.commandWorked(routerConn.getCollection(kCollName).insert({x: -10001}));
    assert.commandWorked(routerConn.getCollection(kCollName).insert({x: 0}));
    assert.commandWorked(routerConn.getCollection(kCollName).insert({x: 10001}));
    let suspendRangeDeletionShard0Fp =
        configureFailPoint(st.rs0.getPrimary(), "suspendRangeDeletion");
    // Set up a sharded collection with a chunk and a document on each shard.
    assert.commandWorked(routerConn.adminCommand({split: kNss, middle: {x: -10000}}));
    assert.commandWorked(routerConn.adminCommand({split: kNss, middle: {x: 10000}}));
    assert.commandWorked(
        routerConn.adminCommand({moveChunk: kNss, find: {x: -10000}, to: kShard1Name}));
    suspendRangeDeletionShard0Fp.wait();
    assert.commandWorked(
        routerConn.adminCommand({moveChunk: kNss, find: {x: 10000}, to: kShard2Name}));
    suspendRangeDeletionShard0Fp.wait();
    // Open a cursor with a comment to be consumed on the shard directly.
    assert.commandWorked(routerConn.runCommand(
        {"find": kCollName, batchSize: 0, sort: {$natural: 1}, comment: cursorComment}));
    return suspendRangeDeletionShard0Fp;
}

function createCursors(routerConn, cursorComment, setupShardConn) {
    let shardCursors =
        routerConn
            .aggregate([
                {$currentOp: {idleCursors: true}},
                {$match: {type: "idleCursor", "cursor.originatingCommand.comment": cursorComment}}
            ])
            .map(x => {
                // By using the comment as an identifier, open a cursor directly on each
                // shard which should take into consideration ownership of data.
                let kDbName = x.ns.split(".", 1);
                let cursor = new DBCommandCursor(
                    setupShardConn(x.host, kDbName),
                    {ok: 1, cursor: {id: x.cursor.cursorId, ns: x.ns, firstBatch: []}});
                return cursor;
            });
    return shardCursors;
}

jsTestLog('Check the fast export procedure without auth');
{
    let kDbName = 'db';
    let kCollName = 'coll';
    const kNss = kDbName + '.' + kCollName;
    let st = new ShardingTest({shards: 3});
    let suspendRangeDeletionShard0Fp =
        setupCluster(st, st.s.getDB(kDbName), kDbName, kCollName, kNss, kNss);
    function setupShardConn(host, kDbName) {
        let conn = new Mongo(host);
        return conn.getDB(kDbName);
    }
    let shardCursors = createCursors(st.s.getDB('admin'), kNss, setupShardConn);
    assert.eq(1, shardCursors[0].toArray().length);
    assert.eq(1, shardCursors[1].toArray().length);
    assert.eq(1, shardCursors[2].toArray().length);
    suspendRangeDeletionShard0Fp.off();
    st.stop();
}

jsTestLog('Check the fast export procedure with auth');
{
    let kDbName = 'db';
    let kCollName = 'coll';
    const kNss = kDbName + '.' + kCollName;
    let st = new ShardingTest({
        shards: 3,
        keyFile: 'jstests/libs/key1',
        other: {mongosOptions: {auth: null}, configOptions: {auth: null}, rsOptions: {auth: null}}
    });
    let admin = st.s.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'password', roles: jsTest.adminUserRoles});
    admin.auth('admin', 'password');
    let suspendRangeDeletionShard0Fp =
        setupCluster(st, admin.getSiblingDB(kDbName), kDbName, kCollName, kNss, kNss);
    function setupShardConn(host, kDbName) {
        let conn = new Mongo(host);
        let local = conn.getDB('local');
        local.auth('__system', 'foopdedoop');
        return local.getSiblingDB(kDbName);
    }
    let shardCursors = createCursors(admin, kNss, setupShardConn);
    assert.eq(1, shardCursors[0].toArray().length);
    assert.eq(1, shardCursors[1].toArray().length);
    assert.eq(1, shardCursors[2].toArray().length);
    suspendRangeDeletionShard0Fp.off();
    st.stop();
}
