/**
 * Tests the auto split will be triggered when using write commands.
 */
(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 1, other: {chunkSize: 1, enableAutoSplit: true}});

var configDB = st.s.getDB('config');

var doc1k = (new Array(1024)).join('x');
var testDB = st.s.getDB('test');

function testSingleBatchInsertShouldAutoSplit() {
    jsTest.log('Test single batch insert should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.insert', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.insert").itcount());

    // This should result in a little over 3MB inserted into the chunk, so with
    // a max chunk size of 1MB we'd expect the autosplitter to split this into
    // at least 3 chunks
    for (var x = 0; x < 3100; x++) {
        assert.commandWorked(testDB.runCommand({
            insert: 'insert',
            documents: [{x: x, v: doc1k}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    waitForOngoingChunkSplits(st);

    // Inserted batch is a multiple of the chunkSize, expect the chunks to split into
    // more than 2.
    assert.gt(findChunksUtil.findChunksByNs(configDB, "test.insert").itcount(), 2);
    testDB.dropDatabase();

    jsTest.log('Test single batch update should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.update', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.update").itcount());

    for (var x = 0; x < 2100; x++) {
        assert.commandWorked(testDB.runCommand({
            update: 'update',
            updates: [{q: {x: x}, u: {x: x, v: doc1k}, upsert: true}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    waitForOngoingChunkSplits(st);

    assert.gt(findChunksUtil.findChunksByNs(configDB, "test.update").itcount(), 1);
}

function testSingleDeleteShouldNotAutoSplit() {
    jsTest.log('Test single delete should not auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.delete', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.delete").itcount());

    for (var x = 0; x < 1100; x++) {
        assert.commandWorked(testDB.runCommand({
            delete: 'delete',
            deletes: [{q: {x: x, v: doc1k}, limit: NumberInt(0)}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    // If we are autosplitting (which we shouldn't be), we want to wait until
    // it's finished, otherwise we could falsely think no autosplitting was
    // done when really it was just in progress.
    waitForOngoingChunkSplits(st);

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.delete").itcount());
}

function testBatchedInsertShouldAutoSplit() {
    jsTest.log('Test batched insert should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.insert', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.insert").itcount());

    // Note: Estimated 'chunk size' tracked by mongos is initialized with a random value so
    // we are going to be conservative.
    for (var x = 0; x < 2100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            docs.push({x: (x + y), v: doc1k});
        }

        assert.commandWorked(testDB.runCommand(
            {insert: 'insert', documents: docs, ordered: false, writeConcern: {w: 1}}));
    }

    waitForOngoingChunkSplits(st);

    assert.gt(findChunksUtil.findChunksByNs(configDB, "test.insert").itcount(), 1);
}

function testBatchedUpdateShouldAutoSplit() {
    jsTest.log('Test batched update should auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.update', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.update").itcount());

    for (var x = 0; x < 2100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            var id = x + y;
            docs.push({q: {x: id}, u: {x: id, v: doc1k}, upsert: true});
        }

        assert.commandWorked(testDB.runCommand(
            {update: 'update', updates: docs, ordered: false, writeConcern: {w: 1}}));
    }

    waitForOngoingChunkSplits(st);

    assert.gt(findChunksUtil.findChunksByNs(configDB, "test.update").itcount(), 1);
}

function testBatchedDeleteShouldNotAutoSplit() {
    jsTest.log('Test batched delete should not auto-split');

    assert.commandWorked(configDB.adminCommand({enableSharding: 'test'}));
    assert.commandWorked(configDB.adminCommand({shardCollection: 'test.delete', key: {x: 1}}));

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.delete").itcount());

    for (var x = 0; x < 2100; x += 400) {
        var docs = [];

        for (var y = 0; y < 400; y++) {
            var id = x + y;
            docs.push({q: {x: id, v: doc1k}, top: 0});
        }

        assert.commandWorked(testDB.runCommand({
            delete: 'delete',
            deletes: [{q: {x: x, v: doc1k}, limit: NumberInt(0)}],
            ordered: false,
            writeConcern: {w: 1}
        }));
    }

    // If we are autosplitting (which we shouldn't be), we want to wait until
    // it's finished, otherwise we could falsely think no autosplitting was
    // done when really it was just in progress.
    waitForOngoingChunkSplits(st);

    assert.eq(1, findChunksUtil.findChunksByNs(configDB, "test.delete").itcount());
}

var testCases = [
    testSingleBatchInsertShouldAutoSplit,
    testSingleDeleteShouldNotAutoSplit,
    testBatchedInsertShouldAutoSplit,
    testBatchedUpdateShouldAutoSplit,
    testBatchedDeleteShouldNotAutoSplit
];

for (let testCase of testCases) {
    try {
        testDB.dropDatabase();
        testCase();
    } catch (e) {
        print("Retrying test case failed due to " + e);
        // (SERVER-59882) The split may not have happened due to write-unit-of-work commit delay
        // Give it another best-effort try, given the low probability it would happen again
        testDB.dropDatabase();
        testCase();
    }
}

st.stop();
})();
