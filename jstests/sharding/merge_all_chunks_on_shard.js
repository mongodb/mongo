/**
 * Tests mergeAllChunksOnShard command
 *
 * @tags: [
 *   featureFlagAutoMerger,
 * ]
 */
(function() {
'use strict';

/* Create new sharded collection on testDB */
let _collCounter = 0;
function newShardedColl(st, testDB) {
    const collNamePrefix = 'coll';
    const coll = testDB[collNamePrefix + '_' + _collCounter++];
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    return coll;
}

/* Split chunk */
function splitChunk(st, coll, splitPointKeyValue) {
    assert.commandWorked(
        st.s.adminCommand({split: coll.getFullName(), middle: {x: splitPointKeyValue}}));
}

/* Move range */
function moveRange(st, coll, minKeyValue, maxKeyValue, toShard) {
    assert.commandWorked(st.s.adminCommand({
        moveRange: coll.getFullName(),
        min: {x: minKeyValue},
        max: {x: maxKeyValue},
        toShard: toShard
    }));
}

/* Set `onCurrentShardSince` field to (refTimestamp + offsetInSeconds) */
function setOnCurrentShardSince(configDB, coll, extraQuery, refTimestamp, offsetInSeconds) {
    const collUuid = configDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, extraQuery);
    const newValue = new Timestamp(refTimestamp.getTime() + offsetInSeconds, 0);
    assert.commandWorked(
        configDB.chunks.updateMany(query, [{
                                       $set: {
                                           "onCurrentShardSince": newValue,
                                           "history": [{validAfter: newValue, shard: "$shard"}]
                                       }
                                   }]));
}

/* Set jumbo flag to true */
function setJumboFlag(configDB, coll, chunkQuery) {
    const collUuid = configDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, chunkQuery);
    assert.commandWorked(configDB.chunks.update(query, {$set: {jumbo: true}}));
}

/* Set history window in all config servers */
let defaultMinSnapshotHistoryWindowInSeconds = null;
let defaultTransactionLifetimeLimitSeconds = null;
function setHistoryWindowInConfigServers(st, valueInSeconds) {
    st.forEachConfigServer((conn) => {
        const res =
            conn.adminCommand({setParameter: 1, minSnapshotHistoryWindowInSeconds: valueInSeconds});
        assert.commandWorked(res);
        defaultMinSnapshotHistoryWindowInSeconds = res.was;
    });
    st.forEachConfigServer((conn) => {
        const res =
            conn.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: valueInSeconds});
        assert.commandWorked(res);
        defaultTransactionLifetimeLimitSeconds = res.was;
    });
}

/* Set back history window to its default value in all config servers */
function setHistoryWindowToDefaultInConfigServers(st) {
    assert(defaultMinSnapshotHistoryWindowInSeconds);
    assert(defaultTransactionLifetimeLimitSeconds);
    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand({
            setParameter: 1,
            minSnapshotHistoryWindowInSeconds: defaultMinSnapshotHistoryWindowInSeconds
        }));
    });
    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: defaultTransactionLifetimeLimitSeconds
        }));
    });
}

/* Verifies that the expected chunks are on the specified shard */
function assertExpectedChunks(configDB, coll, shardName, expectedChunks) {
    const collUuid = configDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const chunks = configDB.chunks.find({uuid: collUuid, shard: shardName}).toArray();
    assert.eq(expectedChunks.length, chunks.length);
    expectedChunks.forEach((expectedChunk) => {
        const chunkFound = chunks.some((chunk) => expectedChunk.min === chunk.min.x &&
                                           expectedChunk.max === chunk.max.x);
        assert(chunkFound,
               "Couldn't find expected range {min: " + tojson(expectedChunk.min) +
                   ", max: " + tojson(expectedChunk.max) + "} on shard " + tojson(shardName));
    });
}

/* Build the following scenario:
 *  - shard0
 *         { min: MinKey, max:0 }
 *         { min: 0,      max:1 }
 *
 *         { min: 3,      max:4 }
 *         { min: 4,      max:5 }
 *         { min: 5,      max:6 }
 *         { min: 6,      max:7 }
 *
 *         { min: 10,     max:MaxKey }
 *
 *  - shard1
 *         { min: 1,      max:2 }
 *         { min: 2,      max:3 }
 *
 *         { min: 7,      max:8 }
 *         { min: 8,      max:9 }
 *         { min: 9,      max:10 }
 *
 *  Note: this function should be called when coll has 1 unique chunk on shard0
 */
function buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds) {
    for (let i = 0; i < 10; i++) {
        splitChunk(st, coll, i);
    }
    moveRange(st, coll, 1, 2, shard1);
    moveRange(st, coll, 2, 3, shard1);
    moveRange(st, coll, 7, 8, shard1);
    moveRange(st, coll, 8, 9, shard1);
    moveRange(st, coll, 9, 10, shard1);

    const expectedChunksPerShard = {
        [shard0]: [
            {min: MinKey, max: 0},
            {min: 0, max: 1},
            {min: 3, max: 4},
            {min: 4, max: 5},
            {min: 5, max: 6},
            {min: 6, max: 7},
            {min: 10, max: MaxKey}
        ],
        [shard1]: [
            {min: 1, max: 2},
            {min: 2, max: 3},
            {min: 7, max: 8},
            {min: 8, max: 9},
            {min: 9, max: 10}
        ]
    };

    const configDB = st.getDB("config");

    assertExpectedChunks(configDB, coll, shard0, expectedChunksPerShard[shard0]);
    assertExpectedChunks(configDB, coll, shard1, expectedChunksPerShard[shard1]);

    const collUuid = configDB.collections.findOne({_id: coll.getFullName()}).uuid;
    return configDB.chunks.findOne({uuid: collUuid}).onCurrentShardSince;
}

/* Tests mergeAllChunks command */
function mergeAllChunksOnShardTest(st, testDB) {
    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInConfigServers(st, historyWindowInSeconds);

    // Distribute deterministically the chunks across the shards(Scenario A)
    const now = buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds);

    // Make sure that all chunks are out of the history window
    setOnCurrentShardSince(configDB, coll, {}, now, -historyWindowInSeconds - 1000);

    // Merge all contiguous chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));
    assertExpectedChunks(
        configDB, coll, shard0, [{min: MinKey, max: 1}, {min: 3, max: 7}, {min: 10, max: MaxKey}]);

    // Merge all contiguous chunks on shard1
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunks(configDB, coll, shard1, [{min: 1, max: 3}, {min: 7, max: 10}]);

    // Set back snapshot history window to its default value
    setHistoryWindowToDefaultInConfigServers(st);
}

/* Tests mergeAllChunks command considering history window preservation */
function mergeAllChunksOnShardConsideringHistoryWindowTest(st, testDB) {
    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history windows to 1 min
    const historyWindowInSeconds = 1000;
    setHistoryWindowInConfigServers(st, 1000);

    // Distribute deterministically the chunks across the shards
    const now = buildInitialScenario(st, coll, shard0, shard1);

    // Initially, make all chunks older than history window
    setOnCurrentShardSince(configDB, coll, {}, now, -historyWindowInSeconds - 1000);

    // Perform some move so that those chunks will fall inside the history window and won't be able
    // to be merged
    moveRange(st, coll, 1, 2, shard0);
    moveRange(st, coll, 2, 3, shard0);

    // Try to merge all contiguous chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks must be merged except{min: 1, max: 2} and{min: 2, max: 3} because they
    // must be preserved when history widow is still active on them
    assertExpectedChunks(configDB, coll, shard0, [
        {min: MinKey, max: 1},
        {min: 1, max: 2},
        {min: 2, max: 3},
        {min: 3, max: 7},
        {min: 10, max: MaxKey}
    ]);

    // Try to merge all contiguous chunks on shard1 and check expected results
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunks(configDB, coll, shard1, [{min: 7, max: 10}]);

    // Set back history windows to its default value
    setHistoryWindowToDefaultInConfigServers(st);
}

/* Tests mergeAllChunks command considering jumbo flag */
function mergeAllChunksOnShardConsideringJumboFlagTest(st, testDB) {
    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInConfigServers(st, historyWindowInSeconds);

    // Distribute deterministically the chunks across the shards
    const now = buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds);

    // Make sure that all chunks are out of the history window
    setOnCurrentShardSince(configDB, coll, {}, now, -historyWindowInSeconds - 1000);

    // Set jumbo flag to a couple of chunks
    // Setting a chunks as jumbo must prevent it from being merged
    setJumboFlag(configDB, coll, {min: {x: 3}});
    setJumboFlag(configDB, coll, {min: {x: 8}});

    // Try to merge all contiguous chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks should be merged except {min: 3, max: 4}
    assertExpectedChunks(
        configDB,
        coll,
        shard0,
        [{min: MinKey, max: 1}, {min: 3, max: 4}, {min: 4, max: 7}, {min: 10, max: MaxKey}]);

    // Try to merge all contiguous chunks on shard1
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));

    // All chunks should be merged except {min: 8, max: 9}
    assertExpectedChunks(configDB,
                         coll,
                         shard1,
                         [{min: 1, max: 3}, {min: 7, max: 8}, {min: 8, max: 9}, {min: 9, max: 10}]);

    // Set back history windows to its default value
    setHistoryWindowToDefaultInConfigServers(st);
}

/* Test setup */
const st = new ShardingTest({mongos: 1, shards: 2, other: {enableBalancer: false}});
const testDB = st.s.getDB(jsTestName());

// Ensure primary shard is shard0
assert.commandWorked(
    st.s.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

/* Perform tests */
mergeAllChunksOnShardTest(st, testDB);

mergeAllChunksOnShardConsideringHistoryWindowTest(st, testDB);
mergeAllChunksOnShardConsideringJumboFlagTest(st, testDB);

st.stop();
})();
