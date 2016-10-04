// Basic integration tests for the background job that periodically kills idle cursors, in both
// mongod and mongos.  This test creates the following four cursors:
//
// 1. A no-timeout cursor through mongos.
// 2. A no-timeout cursor through mongod.
// 3. A normal cursor through mongos.
// 4. A normal cursor through mongod.
//
// After a period of inactivity, the test asserts that cursors #1 and #2 are still alive, and that
// #3 and #4 have been killed.
(function() {
    'use strict';

    var st = new ShardingTest({
        shards: 2,
        other: {
            chunkSize: 1,
            shardOptions: {
                verbose: 1,
                setParameter: {cursorTimeoutMillis: 1000, clientCursorMonitorFrequencySecs: 1}
            },
            mongosOptions: {
                verbose: 1,
                setParameter: {cursorTimeoutMillis: 1000, clientCursorMonitorFrequencySecs: 1}
            }
        }
    });

    var adminDB = st.admin;
    var configDB = st.config;
    var coll = st.s.getDB('test').user;

    assert.commandWorked(adminDB.runCommand({enableSharding: coll.getDB().getName()}));
    st.ensurePrimaryShard(coll.getDB().getName(), 'shard0001');
    assert.commandWorked(adminDB.runCommand({shardCollection: coll.getFullName(), key: {x: 1}}));

    var data = 'c';
    for (var x = 0; x < 18; x++) {
        data += data;
    }

    for (x = 0; x < 200; x++) {
        coll.insert({x: x, v: data});
    }

    var chunkDoc = configDB.chunks.findOne();
    var chunkOwner = chunkDoc.shard;
    var toShard = configDB.shards.findOne({_id: {$ne: chunkOwner}})._id;
    var cmd =
        {moveChunk: coll.getFullName(), find: chunkDoc.min, to: toShard, _waitForDelete: true};
    var res = adminDB.runCommand(cmd);

    jsTest.log('move result: ' + tojson(res));

    var shardedCursorWithTimeout = coll.find();
    var shardedCursorWithNoTimeout = coll.find();
    shardedCursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

    // Query directly to mongod
    var shardHost = configDB.shards.findOne({_id: chunkOwner}).host;
    var mongod = new Mongo(shardHost);
    var shardColl = mongod.getCollection(coll.getFullName());

    var cursorWithTimeout = shardColl.find();
    var cursorWithNoTimeout = shardColl.find();
    cursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

    shardedCursorWithTimeout.next();
    shardedCursorWithNoTimeout.next();

    cursorWithTimeout.next();
    cursorWithNoTimeout.next();

    // Wait until the idle cursor background job has killed the cursors that do not have the "no
    // timeout" flag set.  We use the "cursorTimeoutMillis" and "clientCursorMonitorFrequencySecs"
    // setParameters above to reduce the amount of time we need to wait here.
    assert.soon(function() {
        return coll.getDB().serverStatus().metrics.cursor.timedOut > 0;
    }, "sharded cursor failed to time out", 5000);

    // Wait for the shard to have two open cursors on it (shardedCursorWithNoTimeout and
    // cursorWithNoTimeout).
    // We cannot reliably use metrics.cursor.timedOut here, because this will be 2 if
    // shardedCursorWithTimeout is killed for timing out on the shard, and 1 if
    // shardedCursorWithTimeout is killed by a killCursors command from the mongos.
    assert.soon(function() {
        return shardColl.getDB().serverStatus().metrics.cursor.open.total == 2;
    }, "cursor failed to time out", 5000);

    assert.throws(function() {
        shardedCursorWithTimeout.itcount();
    });
    assert.throws(function() {
        cursorWithTimeout.itcount();
    });

    // +1 because we already advanced once
    assert.eq(coll.count(), shardedCursorWithNoTimeout.itcount() + 1);

    assert.eq(shardColl.count(), cursorWithNoTimeout.itcount() + 1);

    st.stop();
})();
