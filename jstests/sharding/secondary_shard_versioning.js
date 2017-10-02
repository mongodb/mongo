/**
 * Tests that secondaries participate in the shard versioning protocol.
 */
(function() {
    "use strict";

    load('jstests/libs/profiler.js');  // for profilerHasSingleMatchingEntryOrThrow()

    // Set the secondaries to priority 0 and votes 0 to prevent the primaries from stepping down.
    let rsOpts = {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]};
    let st = new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts}});

    assert.commandWorked(st.s0.adminCommand({enableSharding: 'test'}));
    st.ensurePrimaryShard('test', st.shard0.shardName);

    assert.commandWorked(st.s0.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: 'test.foo', middle: {x: 0}}));

    let freshMongos = st.s0;
    let staleMongos = st.s1;

    jsTest.log("do insert from stale mongos to make it load the routing table before the move");
    assert.writeOK(staleMongos.getDB('test').foo.insert({x: 1}));

    jsTest.log("do moveChunk from fresh mongos");
    assert.commandWorked(freshMongos.adminCommand({
        moveChunk: 'test.foo',
        find: {x: 0},
        to: st.shard1.shardName,
    }));

    // Turn on system profiler on secondaries to collect data on all future operations on the db.
    let donorShardSecondary = st.rs0.getSecondary();
    let recipientShardSecondary = st.rs1.getSecondary();
    assert.commandWorked(donorShardSecondary.getDB('test').setProfilingLevel(2));
    assert.commandWorked(recipientShardSecondary.getDB('test').setProfilingLevel(2));

    // Use the mongos with the stale routing table to send read requests to the secondaries. 'local'
    // read concern level must be specified in the request because secondaries default to
    // 'available', which doesn't participate in the version protocol. Check that the donor shard
    // returns a stale shardVersion error, which provokes mongos to refresh its routing table and
    // re-target; that the recipient shard secondary refreshes its routing table on hearing the
    // fresh version from mongos; and that the recipient shard secondary returns the results.

    jsTest.log("do secondary read from stale mongos");
    let res = staleMongos.getDB('test').runCommand({
        count: 'foo',
        query: {x: 1},
        $readPreference: {mode: "secondary"},
        readConcern: {"level": "local"}
    });
    assert(res.ok);
    assert.eq(1, res.n, tojson(res));

    // Check that the donor shard secondary returned stale shardVersion.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: donorShardSecondary.getDB('test'),
        filter: {
            "ns": "test.foo",
            "command.count": "foo",
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern": {"level": "local"},
            "exceptionCode": ErrorCodes.StaleConfig
        }
    });

    // The recipient shard secondary will also return stale shardVersion once, even though the
    // mongos is fresh, because the recipient shard secondary was stale.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: donorShardSecondary.getDB('test'),
        filter: {
            "ns": "test.foo",
            "command.count": "foo",
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern": {"level": "local"},
            "exceptionCode": ErrorCodes.StaleConfig
        }
    });

    // Check that the recipient shard secondary received the query and returned results.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: recipientShardSecondary.getDB('test'),
        filter: {
            "ns": "test.foo",
            "command.count": "foo",
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern": {"level": "local"},
            "exceptionCode": {"$exists": false}
        }
    });

    st.stop();
})();
