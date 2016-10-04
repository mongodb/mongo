//
// Utilities for testing cleanupOrphaned command.
//

//
// Run cleanupOrphaned on a shard, and assert cleanupOrphaned runs the
// expected number of times before stopping.
//
function cleanupOrphaned(shardConnection, ns, expectedIterations) {
    var admin = shardConnection.getDB('admin'), result = admin.runCommand({cleanupOrphaned: ns}),
        iterations = 1;

    if (!result.ok) {
        printjson(result);
    }
    assert(result.ok);
    while (result.stoppedAtKey) {
        result = admin.runCommand({cleanupOrphaned: ns, startingFromKey: result.stoppedAtKey});

        assert(result.ok);
        ++iterations;
    }

    assert.eq(iterations,
              expectedIterations,
              'Expected to run ' +
                  'cleanupOrphaned' + expectedIterations + ' times, but it only ran ' + iterations +
                  ' times before stoppedAtKey was null.');
}

// Shards data from key range, then inserts orphan documents, runs cleanupOrphans
// and makes sure that orphans are removed.
// Pass an options object like:
// {
//     shardKey: { a: 1, b: 1 },
//     keyGen: function() { return [{ a: 'foo', b: 1 }, { a: 'bar', b: 2 }]; }
// }
function testCleanupOrphaned(options) {
    var st = new ShardingTest({shards: 2, mongos: 2});

    var mongos = st.s0, admin = mongos.getDB('admin'),
        shards = mongos.getCollection('config.shards').find().toArray(),
        coll = mongos.getCollection('foo.bar'),
        shard0Coll = st.shard0.getCollection(coll.getFullName()), keys = options.keyGen(),
        beginning = keys[0], oneQuarter = keys[Math.round(keys.length / 4)],
        middle = keys[Math.round(keys.length / 2)],
        threeQuarters = keys[Math.round(3 * keys.length / 4)];

    assert.commandWorked(admin.runCommand({enableSharding: coll.getDB().getName()}));

    printjson(admin.runCommand({movePrimary: coll.getDB() + "", to: shards[0]._id}));

    assert.commandWorked(
        admin.runCommand({shardCollection: coll.getFullName(), key: options.shardKey}));

    st.printShardingStatus();

    jsTest.log('Inserting some regular docs...');

    assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: middle}));

    assert.commandWorked(admin.runCommand(
        {moveChunk: coll.getFullName(), find: middle, to: shards[1]._id, _waitForDelete: true}));

    for (var i = 0; i < keys.length; i++)
        coll.insert(keys[i]);
    assert.eq(null, coll.getDB().getLastError());

    // Half of the data is on each shard:
    // shard 0: [beginning, middle)
    // shard 1:            [middle, end)
    //
    assert.eq(keys.length / 2, shard0Coll.count());
    assert.eq(keys.length, coll.find().itcount());

    jsTest.log('Inserting some orphaned docs...');

    shard0Coll.insert(threeQuarters);

    // I'll represent the orphan doc like {threeQuarters}, in this diagram:
    //
    // shard 0: [beginning, middle) {threeQuarters}
    // shard 1:            [middle,                 end)
    assert.eq(null, shard0Coll.getDB().getLastError());
    assert.eq(1 + keys.length / 2, shard0Coll.count());

    jsTest.log('Cleaning up orphaned data...');

    cleanupOrphaned(st.shard0, coll.getFullName(), 2);
    assert.eq(keys.length / 2, shard0Coll.count());
    assert.eq(keys.length, coll.find().itcount());

    jsTest.log('Moving half the data out again (making a hole)...');

    assert.commandWorked(admin.runCommand({split: coll.getFullName(), middle: oneQuarter}));

    assert.commandWorked(admin.runCommand(
        {moveChunk: coll.getFullName(), find: beginning, to: shards[1]._id, _waitForDelete: true}));

    // 1/4 of the data is on the first shard.
    // shard 0:            [threeQuarters,  middle)
    // shard 1: [beginning, threeQuarters) [middle, end)
    assert.eq(Math.round(keys.length / 4), shard0Coll.count());
    assert.eq(keys.length, coll.find().itcount());

    jsTest.log('Inserting some more orphaned docs...');

    shard0Coll.insert(beginning);
    shard0Coll.insert(middle);
    assert.eq(null, shard0Coll.getDB().getLastError());

    // shard 0: {beginning} [threeQuarters,  middle) {middle}
    // shard 1: [beginning,  threeQuarters)          [middle, end)
    assert.eq(2 + Math.round(keys.length / 4), shard0Coll.count());
    assert.eq(100, coll.find().itcount());

    jsTest.log('Cleaning up more orphaned data...');

    // Now cleanupOrphaned must iterate over 3 regions, not 2.
    cleanupOrphaned(st.shard0, coll.getFullName(), 3);
    assert.eq(Math.round(keys.length / 4), shard0Coll.count());
    assert.eq(keys.length, coll.find().itcount());

    jsTest.log('DONE!');

    st.stop();
}
