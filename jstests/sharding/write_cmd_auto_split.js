/**
 * Tests the auto split will be triggered when using write commands.
 */
(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, other: {chunkSize: 1}});

    var configDB = st.s.getDB('config');
    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.insert', key: {x: 1}}));

    var doc1k = (new Array(1024)).join('x');
    var testDB = st.s.getDB('test');

    jsTest.log('Test single batch insert should auto-split');

    assert.eq(1, configDB.chunks.find().itcount());

    // Note: Estimated 'chunk size' tracked by mongos is initialized with a random value so
    // we are going to be conservative.
    for (var x = 0; x < 3100; x++) {
        assert.writeOK(testDB.runCommand({
            insert: 'insert',
            documents: [{x: x, v: doc1k}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    // Inserted batch is a multiple of the chunkSize, expect the chunks to split into
    // more than 2.
    assert.gt(configDB.chunks.find().itcount(), 2);
    testDB.dropDatabase();

    jsTest.log('Test single batch update should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.update', key: {x: 1}}));

    assert.eq(1, configDB.chunks.find().itcount());

    for (var x = 0; x < 1100; x++) {
        assert.writeOK(testDB.runCommand({
            update: 'update',
            updates: [{q: {x: x}, u: {x: x, v: doc1k}, upsert: true}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    assert.gt(configDB.chunks.find().itcount(), 1);
    testDB.dropDatabase();

    jsTest.log('Test single delete should not auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.delete', key: {x: 1}}));

    assert.eq(1, configDB.chunks.find().itcount());

    for (var x = 0; x < 1100; x++) {
        assert.writeOK(testDB.runCommand({
            delete: 'delete',
            deletes: [{q: {x: x, v: doc1k}, limit: NumberInt(0)}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    assert.eq(1, configDB.chunks.find().itcount());
    testDB.dropDatabase();

    jsTest.log('Test batched insert should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.insert', key: {x: 1}}));

    assert.eq(1, configDB.chunks.find().itcount());

    // Note: Estimated 'chunk size' tracked by mongos is initialized with a random value so
    // we are going to be conservative.
    for (var x = 0; x < 1100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            docs.push({x: (x + y), v: doc1k});
        }

        assert.writeOK(testDB.runCommand(
            {insert: 'insert', documents: docs, ordered: false, writeConcern: {w: 1}}));
    }

    assert.gt(configDB.chunks.find().itcount(), 1);
    testDB.dropDatabase();

    jsTest.log('Test batched update should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.update', key: {x: 1}}));

    assert.eq(1, configDB.chunks.find().itcount());

    for (var x = 0; x < 1100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            var id = x + y;
            docs.push({q: {x: id}, u: {x: id, v: doc1k}, upsert: true});
        }

        assert.writeOK(testDB.runCommand(
            {update: 'update', updates: docs, ordered: false, writeConcern: {w: 1}}));
    }

    assert.gt(configDB.chunks.find().itcount(), 1);
    testDB.dropDatabase();

    jsTest.log('Test batched delete should not auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.delete', key: {x: 1}}));

    assert.eq(1, configDB.chunks.find().itcount());

    for (var x = 0; x < 1100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            var id = x + y;
            docs.push({q: {x: id, v: doc1k}, top: 0});
        }

        assert.writeOK(testDB.runCommand({
            delete: 'delete',
            deletes: [{q: {x: x, v: doc1k}, limit: NumberInt(0)}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    assert.eq(1, configDB.chunks.find().itcount());

    st.stop();
})();
