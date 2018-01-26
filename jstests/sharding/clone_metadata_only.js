'use strict';

// Test that the 'clone' command's metadataOnly option works correctly.
// Eventually, configsvrMovePrimary will exercise this option.

(() => {
    var st = new ShardingTest({shards: 2}), testDB = st.s.getDB('test');

    // Enable sharding on the test DB.
    assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));

    // Create two collections with non-default options.
    var coll1Options = {capped: true, size: 5000000},
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

    // Create views on each collection.
    var view1Pipeline = {$project: {a: 1}},
        view1Collation = {collation: {locale: 'fr', backwards: true}},
        view2Pipeline = {$project: {d: {$add: ['$a', '$b', '$c']}}},
        view3Pipeline = {$project: {c: 1}},
        view3Collation = {collation: {locale: 'fr', backwards: true}},
        view4Pipeline = {$project: {d: {$divide: ['$b', '$a']}}};

    assert.commandWorked(testDB.createView('view1', 'coll1', view1Pipeline, view1Collation));
    assert.commandWorked(testDB.createView('view2', 'coll1', view2Pipeline));
    assert.commandWorked(testDB.createView('view3', 'coll2', view3Pipeline, view3Collation));
    assert.commandWorked(testDB.createView('view4', 'coll2', view4Pipeline));

    // Shard coll2, but leave coll1 unsharded.
    assert.commandWorked(st.s.adminCommand({shardCollection: 'test.coll2', key: {_id: 1}}));

    // Get the primary shard, and the non-primary shard.
    var fromShard = st.getPrimaryShard('test');
    var toShard = st.getOther(fromShard);

    // Have the other shard clone the DB from the primary, using the metadataOnly option.
    assert.commandWorked(
        toShard.getDB('test').runCommand({clone: fromShard.host, 'metadataOnly': true}));

    // Ask the shard that just called clone for the collections, views, and indexes.
    var res = toShard.getDB('test').runCommand({listCollections: 1});
    assert.commandWorked(res);
    // Remove system.indexes collection (which exists in mmap)
    // Indexes are checked separately below.
    var collections = res.cursor.firstBatch.filter(coll => coll.name != 'system.indexes');

    function sortByName(a, b) {
        if (a.name < b.name)
            return -1;
        if (a.name > b.name)
            return 1;
        return 0;
    }

    // Sort collections by name.
    collections.sort(sortByName);

    // There should be 7 collections: coll1, coll2, system.views, view1, view2, view3, view4.
    assert.eq(collections.length, 7);

    var c1, c2, c3, c4, c5, c6, c7;
    [c1, c2, c3, c4, c5, c6, c7] = collections;

    function checkName(c, expectedName) {
        assert.eq(
            c.name, expectedName, 'Expected collection to be ' + expectedName + ', got ' + c.name);
    }

    function checkOptions(c, expectedOptions) {
        assert.hasFields(c, ['options'], 'Missing options field for collection ' + c.name);
        assert.hasFields(
            c.options, expectedOptions, 'Missing expected option(s) for collection ' + c.name);
    }

    // c1 should be coll1.
    checkName(c1, 'coll1');
    checkOptions(c1, Object.keys(coll1Options));

    // c2 should be coll2.
    checkName(c2, 'coll2');
    checkOptions(c2, Object.keys(coll2Options));

    // c3 should be system.views.
    checkName(c3, 'system.views');
    checkOptions(c3, []);

    // c4 should be view1.
    checkName(c4, 'view1');
    checkOptions(c4, ['viewOn', 'pipeline', 'collation']);

    // c5 should be view2.
    checkName(c5, 'view2');
    checkOptions(c5, ['viewOn', 'pipeline']);

    // c6 should be view3.
    checkName(c6, 'view3');
    checkOptions(c6, ['viewOn', 'pipeline', 'collation']);

    // c7 should be view4.
    checkName(c7, 'view4');
    checkOptions(c7, ['viewOn', 'pipeline']);

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

    // Verify that the data from the collections still resides on the primary shard,
    // and was not copied as part of the clone.
    function checkCount(shard, collName, count) {
        var res = shard.getDB('test').runCommand({count: collName});
        assert.commandWorked(res);
        assert.eq(res.n, count);
    }

    checkCount(fromShard, 'coll1', 3);
    checkCount(fromShard, 'coll2', 3);
    checkCount(toShard, 'coll1', 0);
    checkCount(toShard, 'coll2', 0);

    // Check that clone doesn't error when the collections already exist on the destination
    // and metadataOnly = true.
    assert.commandWorked(
        toShard.getDB('test').runCommand({clone: fromShard.host, 'metadataOnly': true}));

    // Now change an option on the toShard, and verify that calling clone again fails if the
    // options don't match.
    assert.commandWorked(
        toShard.getDB('test').runCommand({collMod: 'coll1', validationLevel: 'moderate'}));
    assert.commandFailed(
        toShard.getDB('test').runCommand({clone: fromShard.host, 'metadataOnly': true}));

    // TODO SERVER-32847: check that collection UUIDs are correctly copied over.

    st.stop();
})();
