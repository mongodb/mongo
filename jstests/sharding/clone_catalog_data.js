'use strict';

// Test that the 'cloneCatalogData' command works correctly.
// Eventually, _movePrimary will use this command.

(() => {

    function sortByName(a, b) {
        if (a.name < b.name)
            return -1;
        if (a.name > b.name)
            return 1;
        return 0;
    }

    var st = new ShardingTest({shards: 2}), testDB = st.s.getDB('test');

    // Enable sharding on the test DB.
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

    // Create two collections with non-default options.
    var coll1Options = {capped: true, size: 500},
        coll2Options = {validator: {$jsonSchema: {required: ['a']}}};

    assert.commandWorked(testDB.createCollection('coll1', coll1Options));
    assert.commandWorked(testDB.createCollection('coll2', coll2Options));

    // Create some test documents and put them in each collection.
    [{a: 1, b: 2, c: 4}, {a: 2, b: 4, c: 8}, {a: 3, b: 6, c: 12}].forEach(d => {
        assert.writeOK(testDB.coll1.insert(d));
        assert.writeOK(testDB.coll2.insert(d));
    });

    // Create indexes on each collection.
    var coll1Indexes =
            [
              {key: {a: 1}, name: 'index1', expireAfterSeconds: 5000},
              {key: {b: -1}, name: 'index2', unique: true},
            ],
        coll2Indexes = [
            {key: {a: 1, b: 1}, name: 'index3'},
            {key: {c: 1}, name: 'index4'},
        ];

    assert.commandWorked(testDB.runCommand({createIndexes: 'coll1', indexes: coll1Indexes}));
    assert.commandWorked(testDB.runCommand({createIndexes: 'coll2', indexes: coll2Indexes}));

    // Shard coll2, but leave coll1 unsharded.
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.coll2', key: {_id: 1}}));

    // Wait for the write to config.collections to be visible in the majority snapshot on all config
    // secondaries, since the test directly talks to shards and so does not gossip the configOpTime
    // to those shards.
    st.configRS.awaitLastOpCommitted();

    // Get the primary shard, and the non-primary shard.
    var fromShard = st.getPrimaryShard('test');
    var toShard = st.getOther(fromShard);

    var res = fromShard.getDB('test').runCommand({listCollections: 1});
    assert.commandWorked(res);
    var collections = res.cursor.firstBatch;

    collections.sort(sortByName);
    var coll2uuid = collections[1].info.uuid;

    // Have the other shard clone the DB from the primary.
    assert.commandWorked(toShard.adminCommand(
        {_cloneCatalogData: 'test', from: fromShard.host, writeConcern: {w: "majority"}}));

    // Ask the shard that just called _cloneCatalogData for the collections.
    res = toShard.getDB('test').runCommand({listCollections: 1});
    assert.commandWorked(res);

    collections = res.cursor.firstBatch;

    // There should be 2 collections: coll1, coll2
    assert.eq(collections.length, 2);
    collections.sort(sortByName);

    var c1, c2;
    [c1, c2] = collections;

    function checkName(c, expectedName) {
        assert.eq(
            c.name, expectedName, 'Expected collection to be ' + expectedName + ', got ' + c.name);
    }

    function checkOptions(c, expectedOptions) {
        assert.hasFields(c, ['options'], 'Missing options field for collection ' + c.name);
        assert.hasFields(
            c.options, expectedOptions, 'Missing expected option(s) for collection ' + c.name);
    }

    function checkUUID(c, expectedUUID) {
        assert.hasFields(c, ['info'], 'Missing info field for collection ' + c.name);
        assert.hasFields(c.info, ['uuid'], 'Missing uuid field for collection ' + c.name);
        assert.eq(c.info.uuid, expectedUUID, 'Incorrect uuid for collection ' + c.name);
    }

    // c1 should be coll1.
    checkName(c1, 'coll1');
    checkOptions(c1, Object.keys(coll1Options));

    // c2 should be coll2.
    checkName(c2, 'coll2');
    checkOptions(c2, Object.keys(coll2Options));
    checkUUID(c2, coll2uuid);

    function checkIndexes(collName, expectedIndexes) {
        var res = toShard.getDB('test').runCommand({listIndexes: collName});
        assert.commandWorked(res, 'Failed to get indexes for collection ' + collName);
        var indexes = res.cursor.firstBatch;
        indexes.sort(sortByName);

        // There should be 3 indexes on each collection - the _id one, and the 2 we created.
        assert.eq(indexes.length, 3);

        indexes.forEach((index, i) => {
            var expected;
            if (i == 0)
                expected = {name: "_id_", key: {_id: 1}};
            else
                expected = expectedIndexes[i - 1];
            Object.keys(expected).forEach(k => {
                assert.eq(index[k], expected[k]);
            });
        });
    }

    checkIndexes('coll1', coll1Indexes);
    checkIndexes('coll2', coll2Indexes);

    // Verify that the data from the unsharded collections resides on the new primary shard, and was
    // copied as part of the clone.
    function checkCount(shard, collName, count) {
        var res = shard.getDB('test').runCommand({count: collName});
        assert.commandWorked(res);
        assert.eq(res.n, count);
    }

    checkCount(fromShard, 'coll1', 3);
    checkCount(fromShard, 'coll2', 3);
    checkCount(toShard, 'coll1', 3);
    checkCount(toShard, 'coll2', 0);

    // Check that the command fails without writeConcern majority.
    assert.commandFailedWithCode(
        toShard.adminCommand({_cloneCatalogData: 'test', from: fromShard.host}),
        ErrorCodes.InvalidOptions);

    // Check that the command fails when attempting to clone the admin database.
    assert.commandFailedWithCode(
        toShard.adminCommand(
            {_cloneCatalogData: 'admin', from: fromShard.host, writeConcern: {w: "majority"}}),
        ErrorCodes.InvalidOptions);

    // Check that the command fails when attempting to run on the config server.
    assert.commandFailedWithCode(
        st.configRS.getPrimary().adminCommand(
            {_cloneCatalogData: 'test', from: fromShard.host, writeConcern: {w: "majority"}}),
        ErrorCodes.NoShardingEnabled);

    // Check that the command fails when failing to specify a source.
    assert.commandFailedWithCode(
        toShard.adminCommand({_cloneCatalogData: 'test', from: '', writeConcern: {w: "majority"}}),
        ErrorCodes.InvalidOptions);

    // Check that clone errors when the collection already exists on the destination.
    assert.commandFailedWithCode(
        toShard.adminCommand(
            {_cloneCatalogData: 'test', from: fromShard.host, writeConcern: {w: "majority"}}),
        ErrorCodes.NamespaceExists);

    st.stop();
})();
