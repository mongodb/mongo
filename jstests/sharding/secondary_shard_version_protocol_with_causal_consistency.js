/**
 * Tests the shard version protocol on secondaries with causal consistency. A secondary request with
 * read concern level 'available' and afterClusterTime specified should error because they ensure
 * contradictory things. A secondary request with afterClusterTime specified and no read concern
 * level should default to 'local' read concern level, using the shard version protocol.
 */
(function() {
    "use strict";

    load('jstests/libs/profiler.js');  // for profilerHasSingleMatchingEntryOrThrow()

    // Set the secondaries to priority 0 and votes 0 to prevent the primaries from stepping down.
    let rsOpts = {nodes: [{rsConfig: {votes: 1}}, {rsConfig: {priority: 0, votes: 0}}]};
    let st =
        new ShardingTest({mongos: 2, shards: {rs0: rsOpts, rs1: rsOpts}, causallyConsistent: true});
    let dbName = 'test', collName = 'foo', ns = 'test.foo';

    assert.commandWorked(st.s0.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.shardName);

    assert.commandWorked(st.s0.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s0.adminCommand({split: ns, middle: {x: 0}}));

    let freshMongos = st.s0;
    let staleMongos = st.s1;

    jsTest.log("do insert from stale mongos to make it load the routing table before the move");
    assert.writeOK(staleMongos.getCollection(ns).insert({x: 1}));

    jsTest.log("do moveChunk from fresh mongos");
    assert.commandWorked(freshMongos.adminCommand({
        moveChunk: ns,
        find: {x: 0},
        to: st.shard1.shardName,
        secondaryThrottle: true,
        _waitForDelete: true,
        writeConcern: {w: 2},
    }));

    // Turn on system profiler on secondaries to collect data on all future operations on the db.
    let donorShardSecondary = st.rs0.getSecondary();
    let recipientShardSecondary = st.rs1.getSecondary();
    assert.commandWorked(donorShardSecondary.getDB(dbName).setProfilingLevel(2));
    assert.commandWorked(recipientShardSecondary.getDB(dbName).setProfilingLevel(2));

    // Note: this query will not be registered by the profiler because it errors before reaching the
    // storage level.
    jsTest.log("Do a secondary read from stale mongos with afterClusterTime and level 'available'");
    const staleMongosDB = staleMongos.getDB(dbName);
    assert.commandFailedWithCode(staleMongosDB.runCommand({
        count: collName,
        query: {x: 1},
        $readPreference: {mode: "secondary"},
        readConcern: {
            'afterClusterTime': staleMongosDB.getSession().getOperationTime(),
            'level': 'available'
        }
    }),
                                 ErrorCodes.InvalidOptions);

    jsTest.log("Do a secondary read from stale mongos with afterClusterTime and no level");
    let res = staleMongosDB.runCommand({
        count: collName,
        query: {x: 1},
        $readPreference: {mode: "secondary"},
        readConcern: {'afterClusterTime': staleMongosDB.getSession().getOperationTime()},
    });
    assert(res.ok);
    assert.eq(1, res.n, tojson(res));

    // The stale mongos will first go to the donor shard and receive a stale shard version,
    // prompting the stale mongos to refresh it's routing table and retarget to the recipient shard.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: donorShardSecondary.getDB(dbName),
        filter: {
            "ns": ns,
            "command.count": collName,
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern.afterClusterTime": {"$exists": true},
            "exceptionCode": ErrorCodes.StaleConfig
        }
    });

    // The recipient shard will then return a stale shard version error because it needs to refresh
    // its own routing table.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: recipientShardSecondary.getDB(dbName),
        filter: {
            "ns": ns,
            "command.count": collName,
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern.afterClusterTime": {"$exists": true},
            "exceptionCode": ErrorCodes.StaleConfig
        }
    });

    // Finally, the command is retried on the recipient shard and succeeds.
    profilerHasSingleMatchingEntryOrThrow({
        profileDB: recipientShardSecondary.getDB(dbName),
        filter: {
            "ns": ns,
            "command.count": collName,
            "command.query": {x: 1},
            "command.shardVersion": {"$exists": true},
            "command.$readPreference": {"mode": "secondary"},
            "command.readConcern.afterClusterTime": {"$exists": true},
            "exceptionCode": {"$exists": false}
        }
    });

    st.stop();
})();
