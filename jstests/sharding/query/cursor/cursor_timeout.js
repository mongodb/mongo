// Basic integration tests for the background job that periodically kills idle cursors, in both
// mongod and mongos.  This test creates the following cursors:
//
// 1. A no-timeout cursor through mongos, not attached to a session.
// 2. A no-timeout cursor through mongod, not attached to a session.
// 3. A normal cursor through mongos, not attached to a session.
// 4. A normal cursor through mongod, not attached to a session.
// 5. A normal cursor through mongos, attached to a session.
// 6. A normal cursor through mongod, attached to a session.
// 7. A normal aggregation cursor through mongos, not attached to a session.
//
// After a period of inactivity, the test asserts that cursors #1 and #2 are still alive, cursors
// #3, #4, and #7 have been killed, and cursors #5 and #6 are still alive (as of SERVER-6036,
// only cursors not opened as part of a session should be killed by the cursor-timeout mechanism).
// It then kills the session cursors #5 and #6 are attached to simulate that session timing out, and
// ensures that cursors #5 and #6 are killed as a result.
//
// @tags: [
//   requires_fcv_51,
//   requires_sharding,
//   requires_getmore,
// ]
// This test manually simulates a session, which is not compatible with implicit sessions.
import {ShardingTest} from "jstests/libs/shardingtest.js";

TestData.disableImplicitSessions = true;

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
        rsOptions: {
            verbose: 1,
            setParameter: {
                cursorTimeoutMillis: mongodCursorTimeoutMs,
                clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs,
            },
        },
        mongosOptions: {
            verbose: 1,
            setParameter: {
                cursorTimeoutMillis: mongosCursorTimeoutMs,
                clientCursorMonitorFrequencySecs: cursorMonitorFrequencySecs,
            },
        },
    },
    enableBalancer: false,
});

const adminDB = st.admin;
const mongosDB = st.s.getDB("test");
const routerColl = mongosDB.user;

const shardHost = st.config.shards.findOne({_id: st.shard1.shardName}).host;
const mongod = new Mongo(shardHost);
const shardColl = mongod.getCollection(routerColl.getFullName());
const shardDB = shardColl.getDB();

assert.commandWorked(
    adminDB.runCommand({enableSharding: routerColl.getDB().getName(), primaryShard: st.shard0.shardName}),
);

assert.commandWorked(adminDB.runCommand({shardCollection: routerColl.getFullName(), key: {x: 1}}));
assert.commandWorked(adminDB.runCommand({split: routerColl.getFullName(), middle: {x: 10}}));
assert.commandWorked(
    adminDB.runCommand({
        moveChunk: routerColl.getFullName(),
        find: {x: 11},
        to: st.shard1.shardName,
        _waitForDelete: true,
    }),
);

for (let x = 0; x < 20; x++) {
    assert.commandWorked(routerColl.insert({x: x}));
}

// Open both a normal and a no-timeout cursor on mongos. Batch size is 1 to ensure that
// cursor.next() performs only a single operation.
const routerCursorWithTimeout = routerColl.find().batchSize(1);
const routerCursorWithNoTimeout = routerColl.find().batchSize(1);
routerCursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

// Open a session on mongos.
let routerSession = mongosDB.getMongo().startSession();
let routerSessionDB = routerSession.getDatabase(mongosDB.getName());
let routerSessionCursor = routerSessionDB.user.find().batchSize(1);

// Open both a normal and a no-timeout cursor on mongod. Batch size is 1 to ensure that
// cursor.next() performs only a single operation.
const shardCursorWithTimeout = shardColl.find().batchSize(1);
const shardCursorWithNoTimeout = shardColl.find().batchSize(1);
shardCursorWithNoTimeout.addOption(DBQuery.Option.noTimeout);

// Open a session on mongod.
let shardSession = shardDB.getMongo().startSession();
let shardSessionDB = shardSession.getDatabase(shardDB.getName());
let shardSessionCursor = shardSessionDB.user.find().batchSize(1);

// Execute initial find on each cursor.
routerCursorWithTimeout.next();
routerCursorWithNoTimeout.next();
shardCursorWithTimeout.next();
shardCursorWithNoTimeout.next();
routerSessionCursor.next();
shardSessionCursor.next();

// Wait until the idle cursor background job has killed the session-unattached cursors that do not
// have the "no timeout" flag set.  We use the "cursorTimeoutMillis" and
// "clientCursorMonitorFrequencySecs" setParameters above to reduce the amount of time we need to
// wait here.
assert.soon(function () {
    return routerColl.getDB().serverStatus().metrics.cursor.timedOut > 0;
}, "sharded cursor failed to time out");

// Wait for the shard to have four open cursors on it (routerCursorWithNoTimeout,
// routerSessionCursor, shardCursorWithNoTimeout, and shardSessionCursor).  We cannot reliably use
// metrics.cursor.timedOut here, because this will be 2 if routerCursorWithTimeout is killed for
// timing out on the shard, and 1 if routerCursorWithTimeout is killed by a killCursors command from
// the mongos.
assert.soon(function () {
    return shardColl.getDB().serverStatus().metrics.cursor.open.total == 4;
}, "cursor failed to time out");

assert.throws(function () {
    routerCursorWithTimeout.itcount();
});
assert.throws(function () {
    shardCursorWithTimeout.itcount();
});

// Kill the session that routerSessionCursor and shardSessionCursor are attached to, to simulate
// that session's expiration. The cursors should be killed and cleaned up once the session is.
assert.commandWorked(mongosDB.runCommand({killSessions: [routerSession.getSessionId()]}));
assert.commandWorked(shardDB.runCommand({killSessions: [shardSession.getSessionId()]}));

// Wait for the shard to have two open cursors on it (routerCursorWithNoTimeout,
// shardCursorWithNoTimeout).
assert.soon(function () {
    return shardColl.getDB().serverStatus().metrics.cursor.open.total == 2;
}, "session cursor failed to time out");

assert.throws(function () {
    routerSessionCursor.itcount();
});
assert.throws(function () {
    shardSessionCursor.itcount();
});

// Verify that the session cursors are really gone by running a killCursors command, and checking
// that the cursors are reported as "not found". Credit to kill_pinned_cursor_js_test for this
// idea.
let killRes = mongosDB.runCommand({
    killCursors: routerColl.getName(),
    cursors: [routerSessionCursor.getId(), shardSessionCursor.getId()],
});
assert.commandWorked(killRes);
assert.eq(killRes.cursorsAlive, []);
assert.eq(killRes.cursorsNotFound, [routerSessionCursor.getId(), shardSessionCursor.getId()]);
assert.eq(killRes.cursorsUnknown, []);

// +1 because we already advanced one. Ensure that the session-unattached cursors are still valid.
assert.eq(routerColl.count(), routerCursorWithNoTimeout.itcount() + 1);
assert.eq(shardColl.count(), shardCursorWithNoTimeout.itcount() + 1);

// Confirm that cursors opened within a session will timeout when the
// 'enableTimeoutOfInactiveSessionCursors' setParameter has been enabled.
(function () {
    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: true}));
    assert.commandWorked(shardDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: true}));

    // Open a session on mongos.
    routerSession = mongosDB.getMongo().startSession();
    routerSessionDB = routerSession.getDatabase(mongosDB.getName());
    routerSessionCursor = routerSessionDB.user.find().batchSize(1);
    const numRouterCursorsTimedOut = routerColl.getDB().serverStatus().metrics.cursor.timedOut;

    // Open a session on mongod.
    shardSession = shardDB.getMongo().startSession();
    shardSessionDB = shardSession.getDatabase(shardDB.getName());
    shardSessionCursor = shardSessionDB.user.find().batchSize(1);
    const numShardCursorsTimedOut = routerColl.getDB().serverStatus().metrics.cursor.timedOut;

    // Execute initial find on each cursor.
    routerSessionCursor.next();
    shardSessionCursor.next();

    // Wait until mongos reflects the newly timed out cursors.
    assert.soon(function () {
        return shardColl.getDB().serverStatus().metrics.cursor.timedOut >= numRouterCursorsTimedOut + 1;
    }, "sharded cursor failed to time out");

    // Wait until mongod reflects the newly timed out cursors.
    assert.soon(function () {
        return routerColl.getDB().serverStatus().metrics.cursor.timedOut >= numShardCursorsTimedOut + 1;
    }, "router cursor failed to time out");

    assert.throws(function () {
        routerCursorWithTimeout.itcount();
    });
    assert.throws(function () {
        shardCursorWithTimeout.itcount();
    });

    {
        // Test that aggregation cursors on mongos time out properly.

        // Insert some additional data to ensure proper grouping.
        for (let x = 0; x < 30; x++) {
            assert.commandWorked(routerColl.insert({x: x, group: x % 5, value: x * 10}));
        }

        const shard0Count = st.shard0.getDB(mongosDB.getName())[routerColl.getName()].count();
        const shard1Count = st.shard1.getDB(mongosDB.getName())[routerColl.getName()].count();
        assert.gt(shard0Count, 0, "Expected documents on shard0");
        assert.gt(shard1Count, 0, "Expected documents on shard1");

        // Use a small batch size to ensure multiple batches.
        const batchSize = 2;

        // Create aggregation cursors on mongos.
        const routerAggCursor = routerColl.aggregate(
            [{$group: {_id: "$group", count: {$sum: 1}, total: {$sum: "$value"}}}, {$sort: {_id: 1}}],
            {cursor: {batchSize: batchSize}},
        );

        const routerTimeoutCount = routerColl.getDB().serverStatus().metrics.cursor.timedOut;

        assert.soon(function () {
            return routerColl.getDB().serverStatus().metrics.cursor.timedOut > routerTimeoutCount;
        }, "mongos aggregation cursor with $group failed to time out");

        assert.throws(
            function () {
                routerAggCursor.itcount();
            },
            [],
            "Expected mongos aggregation cursor to be timed out",
        );
    }

    assert.commandWorked(mongosDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: false}));
    assert.commandWorked(shardDB.adminCommand({setParameter: 1, enableTimeoutOfInactiveSessionCursors: false}));
})();

st.stop();
