/*
 * Tests that the autosplitter makes at least one split
 */
(function() {
'use strict';
load('jstests/sharding/autosplit_include.js');
load("jstests/sharding/libs/find_chunks_util.js");

var st = new ShardingTest({shards: 1, mongos: 1, other: {chunkSize: 1, enableAutoSplit: true}});

/* Return total number of chunks for a specific collection */
function getNumChunksForColl(coll) {
    return findChunksUtil.countChunksForNs(st.getDB('config'), coll.getFullName());
}

/* Return a collection named @collName sharded on `_id` */
function setupCollection(collName) {
    let coll = db.getCollection(collName);
    st.shardColl(coll, {_id: 1}, false /* split */, false /* move */);
    return coll;
}

const n = 64;
const minChunks = 2;
const big = 'x'.repeat(32768);  // 32 KB
const db = st.getDB('test');
const collPrefix = 'update_and_autosplit_via_';

jsTestLog('Update via findAndModify');
{
    let coll = setupCollection(collPrefix + 'fam');

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < n; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    for (var i = 0; i < n; i++) {
        coll.findAndModify({query: {_id: i}, update: {$set: {big: big}}});
    }

    waitForOngoingChunkSplits(st);
    assert.gte(getNumChunksForColl(coll),
               minChunks,
               "findAndModify update code path didn't result in splits");
}

jsTestLog("Upsert via findAndModify");
{
    let coll = setupCollection(collPrefix + 'fam_upsert');

    for (let i = 0; i < n; i++) {
        coll.findAndModify({query: {_id: i}, update: {$set: {big: big}}, upsert: true});
    }

    waitForOngoingChunkSplits(st);
    assert.gte(getNumChunksForColl(coll),
               minChunks,
               "findAndModify upsert code path didn't result in splits");
}

jsTestLog("Basic update");
{
    let coll = setupCollection(collPrefix + 'update');

    let bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < n; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    for (let i = 0; i < n; i++) {
        assert.commandWorked(coll.update({_id: i}, {$set: {big: big}}));
    }

    waitForOngoingChunkSplits(st);
    assert.gte(getNumChunksForColl(coll), minChunks, "update code path didn't result in splits");
}

jsTestLog("Basic update with upsert");
{
    let coll = setupCollection(collPrefix + 'update_upsert');

    for (var i = 0; i < n; i++) {
        assert.commandWorked(coll.update({_id: i}, {$set: {big: big}}, true));
    }

    waitForOngoingChunkSplits(st);
    assert.gte(getNumChunksForColl(coll), minChunks, "upsert code path didn't result in splits");
}

st.stop();
})();
