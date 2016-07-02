//
// Testing migrations are successful with secondaryThrottle.
//

(function() {
    'use strict';

    // The mongod secondaries are set to priority 0 and votes 0 to prevent the primaries
    // from stepping down during migrations on slow evergreen builders.
    var s = new ShardingTest({
        shards: 2,
        other: {
            chunkSize: 1,
            rs0: {
                nodes: [
                    {rsConfig: {votes: 1}},
                    {rsConfig: {priority: 0, votes: 0}},
                ],
            },
            rs1: {
                nodes: [
                    {rsConfig: {votes: 1}},
                    {rsConfig: {priority: 0, votes: 0}},
                ],
            }
        }
    });

    var coll = s.s0.getCollection("test.foo");
    var bulk = coll.initializeUnorderedBulkOp();
    for (var i = 0; i < 2100; i++) {
        bulk.insert({_id: i, x: i});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(s.s0.adminCommand({enablesharding: coll.getDB() + ""}));
    s.ensurePrimaryShard(coll.getDB() + "", s.shard0.shardName);
    assert.commandWorked(s.s0.adminCommand({shardcollection: coll + "", key: {_id: 1}}));

    for (i = 0; i < 20; i++) {
        assert.commandWorked(s.s0.adminCommand({split: coll + "", middle: {_id: i * 100}}));
    }

    assert.eq(2100, coll.find().itcount());
    coll.setSlaveOk();
    assert.eq(2100, coll.find().itcount());

    for (i = 0; i < 20; i++) {
        // Needs to waitForDelete because we'll be performing a slaveOk query, and secondaries don't
        // have a chunk manager so it doesn't know how to filter out docs it doesn't own.
        assert.commandWorked(s.s0.adminCommand({
            moveChunk: "test.foo",
            find: {_id: i * 100},
            to: s.shard1.shardName,
            _secondaryThrottle: true,
            writeConcern: {w: 2},
            _waitForDelete: true
        }));

        assert.eq(2100, coll.find().itcount());
    }

    s.stop();
}());
