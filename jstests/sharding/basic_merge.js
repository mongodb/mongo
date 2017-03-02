/**
 * Perform basic tests for the mergeChunks command against bongos.
 */
(function() {
    'use strict';

    var st = new ShardingTest({bongos: 2, shards: 2, other: {chunkSize: 1}});
    var bongos = st.s0;

    var kDbName = 'db';

    var shard0 = st.shard0.shardName;
    var shard1 = st.shard1.shardName;

    var ns = kDbName + ".foo";

    assert.commandWorked(bongos.adminCommand({enableSharding: kDbName}));
    st.ensurePrimaryShard(kDbName, shard0);

    // Fail if invalid namespace.
    assert.commandFailed(bongos.adminCommand({mergeChunks: '', bounds: [{a: -1}, {a: 1}]}));

    // Fail if database does not exist.
    assert.commandFailed(bongos.adminCommand({mergeChunks: 'a.b', bounds: [{a: -1}, {a: 1}]}));

    // Fail if collection is unsharded.
    assert.commandFailed(
        bongos.adminCommand({mergeChunks: kDbName + '.xxx', bounds: [{a: -1}, {a: 1}]}));

    // Errors if either bounds is not a valid shard key.
    assert.eq(0, bongos.getDB('config').chunks.count({ns: ns}));

    assert.commandWorked(bongos.adminCommand({shardCollection: ns, key: {a: 1}}));
    assert.eq(1, bongos.getDB('config').chunks.count({ns: ns}));
    assert.commandWorked(bongos.adminCommand({split: ns, middle: {a: 0}}));
    assert.commandWorked(bongos.adminCommand({split: ns, middle: {a: -1}}));
    assert.commandWorked(bongos.adminCommand({split: ns, middle: {a: 1}}));

    // Fail if a wrong key
    assert.commandFailed(bongos.adminCommand({mergeChunks: ns, bounds: [{x: -1}, {a: 1}]}));
    assert.commandFailed(bongos.adminCommand({mergeChunks: ns, bounds: [{a: -1}, {x: 1}]}));

    // Fail if chunks do not contain a bound
    assert.commandFailed(bongos.adminCommand({mergeChunks: ns, bounds: [{a: -1}, {a: 10}]}));

    // Fail if chunks to be merged are not contiguous on the shard
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: ns, bounds: [{a: -1}, {a: 0}], to: shard1, _waitForDelete: true}));
    assert.commandFailed(
        st.s0.adminCommand({mergeChunks: ns, bounds: [{a: MinKey()}, {a: MaxKey()}]}));
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: ns, bounds: [{a: -1}, {a: 0}], to: shard0, _waitForDelete: true}));

    // Validate metadata
    // There are four chunks [{$minKey, -1}, {-1, 0}, {0, 1}, {1, $maxKey}]
    assert.eq(4, st.s0.getDB('config').chunks.count({ns: ns}));

    // Use the second (stale) bongos to invoke the mergeChunks command so we can exercise the stale
    // shard version refresh logic
    assert.commandWorked(st.s1.adminCommand({mergeChunks: ns, bounds: [{a: -1}, {a: 1}]}));
    assert.eq(3, bongos.getDB('config').chunks.count({ns: ns}));
    assert.eq(1, bongos.getDB('config').chunks.count({ns: ns, min: {a: -1}, max: {a: 1}}));

    st.stop();

})();
