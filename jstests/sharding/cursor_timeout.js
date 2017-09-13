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

    // Cursor timeout on mongod is handled by a single thread/timer that will sleep for
    // "clientCursorMonitorFrequencySecs" and add the sleep value to each operation's duration when
    // it wakes up, timing out those whose "now() - last accessed since" time exceeds. A cursor
    // timeout of 5 seconds with a monitor frequency of 1 second means an effective timeout period
    // of 4 to 5 seconds.
    const mongodCursorTimeoutMs = 5000;

    // Cursor timeout on mongos is handled by checking whether the "last accessed" cursor time stamp
    // is older than "now() - cursorTimeoutMillis" and is checked every
    // "clientCursorMonitorFrequencySecs" by a global thread/timer. A timeout of 4 seconds with a
    // monitor frequency of 1 second means an effective timeout period of 4 to 5 seconds.
    const mongosCursorTimeoutMs = 4000;

    const cursorMonitorFrequencySecs = 1;

    const st = new ShardingTest({
        shards: 2,
        other: {
            shardOptions: {
                verbose: 1,
                setParameter: {
                    cursorTimeoutMillis: mongodCursorTimeoutMs,
                    clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs
                }
            },
            mongosOptions: {
                verbose: 1,
                setParameter: {
                    cursorTimeoutMillis: mongosCursorTimeoutMs,
                    clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs
                }
            }
        },
        enableBalancer: false
    });

    const adminDB = st.admin;
    const routerColl = st.s.getDB('test').user;

    const shardHost = st.config.shards.findOne({_id: "shard0001"}).host;
    const mongod = new Mongo(shardHost);
    const shardColl = mongod.getCollection(routerColl.getFullName());

    assert.commandWorked(adminDB.runCommand({enableSharding: routerColl.getDB().getName()}));
    st.ensurePrimaryShard(routerColl.getDB().getName(), 'shard0000');
    assert.commandWorked(
        adminDB.runCommand({shardCollection: routerColl.getFullName(), key: {x: 1}}));
    assert.commandWorked(adminDB.runCommand({split: routerColl.getFullName(), middle: {x: 10}}));
    assert.commandWorked(adminDB.runCommand({
        moveChunk: routerColl.getFullName(),
        find: {x: 11},
        to: "shard0001",
        _waitForDelete: true
    }));

    for (let x = 0; x < 20; x++) {
        assert.writeOK(routerColl.insert({x: x}));
    }

    // Open both a normal and a no-timeout cursor on mongos. Batch size is 1 to ensure that
    // cursor.next() performs only a single operation.
    const routerCursorWithTimeout = routerColl.find().batchSize(1);
    const routerCursorWithNoTimeout = routerColl.find().batchSize(1);
    routerCursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

    // Open both a normal and a no-timeout cursor on mongod. Batch size is 1 to ensure that
    // cursor.next() performs only a single operation.
    const shardCursorWithTimeout = shardColl.find().batchSize(1);
    const shardCursorWithNoTimeout = shardColl.find().batchSize(1);
    shardCursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

    // Execute initial find on each cursor.
    routerCursorWithTimeout.next();
    routerCursorWithNoTimeout.next();
    shardCursorWithTimeout.next();
    shardCursorWithNoTimeout.next();

    // Wait until the idle cursor background job has killed the cursors that do not have the "no
    // timeout" flag set.  We use the "cursorTimeoutMillis" and "clientCursorMonitorFrequencySecs"
    // setParameters above to reduce the amount of time we need to wait here.
    assert.soon(function() {
        return routerColl.getDB().serverStatus().metrics.cursor.timedOut > 0;
    }, "sharded cursor failed to time out");

    // Wait for the shard to have two open cursors on it (routerCursorWithNoTimeout and
    // shardCursorWithNoTimeout).
    // We cannot reliably use metrics.cursor.timedOut here, because this will be 2 if
    // routerCursorWithTimeout is killed for timing out on the shard, and 1 if
    // routerCursorWithTimeout is killed by a killCursors command from the mongos.
    assert.soon(function() {
        return shardColl.getDB().serverStatus().metrics.cursor.open.total == 2;
    }, "cursor failed to time out");

    assert.throws(function() {
        routerCursorWithTimeout.itcount();
    });
    assert.throws(function() {
        shardCursorWithTimeout.itcount();
    });

    // +1 because we already advanced once
    assert.eq(routerColl.count(), routerCursorWithNoTimeout.itcount() + 1);
    assert.eq(shardColl.count(), shardCursorWithNoTimeout.itcount() + 1);

    st.stop();
})();
