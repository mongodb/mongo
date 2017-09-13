/**
 * Testing migrations are successful and immediately visible on the secondaries, when
 * secondaryThrottle is used.
 */
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

    var bulk = s.s0.getDB('TestDB').TestColl.initializeUnorderedBulkOp();
    for (var i = 0; i < 2100; i++) {
        bulk.insert({_id: i, x: i});
    }
    assert.writeOK(bulk.execute());

    assert.commandWorked(s.s0.adminCommand({enablesharding: 'TestDB'}));
    s.ensurePrimaryShard('TestDB', s.shard0.shardName);
    assert.commandWorked(s.s0.adminCommand({shardcollection: 'TestDB.TestColl', key: {_id: 1}}));

    for (i = 0; i < 20; i++) {
        assert.commandWorked(s.s0.adminCommand({split: 'TestDB.TestColl', middle: {_id: i * 100}}));
    }

    var collPrimary = (new Mongo(s.s0.host)).getDB('TestDB').TestColl;
    assert.eq(2100, collPrimary.find().itcount());

    var collSlaveOk = (new Mongo(s.s0.host)).getDB('TestDB').TestColl;
    collSlaveOk.setSlaveOk();
    assert.eq(2100, collSlaveOk.find().itcount());

    for (i = 0; i < 20; i++) {
        // Needs to waitForDelete because we'll be performing a slaveOk query, and secondaries don't
        // have a chunk manager so it doesn't know how to filter out docs it doesn't own.
        assert.commandWorked(s.s0.adminCommand({
            moveChunk: 'TestDB.TestColl',
            find: {_id: i * 100},
            to: s.shard1.shardName,
            _secondaryThrottle: true,
            writeConcern: {w: 2},
            _waitForDelete: true
        }));

        assert.eq(2100,
                  collSlaveOk.find().itcount(),
                  'Incorrect count when reading from secondary. Count from primary is ' +
                      collPrimary.find().itcount());
    }

    s.stop();
}());
