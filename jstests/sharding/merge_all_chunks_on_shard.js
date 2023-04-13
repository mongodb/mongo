/**
 * Tests mergeAllChunksOnShard command and auto-merger behavior
 *
 * @tags: [
 *   requires_fcv_70,
 * ]
 */
(function() {
'use strict';
load("jstests/sharding/libs/find_chunks_util.js");

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
function setOnCurrentShardSince(mongoS, coll, extraQuery, refTimestamp, offsetInSeconds) {
    // Use 'retryWrites' when writing to the configsvr because they are not automatically retried.
    const mongosSession = mongoS.startSession({retryWrites: true});
    const sessionConfigDB = mongosSession.getDatabase('config');
    const collUuid = sessionConfigDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, extraQuery);
    const newValue = new Timestamp(refTimestamp.getTime() + offsetInSeconds, 0);
    assert.commandWorked(sessionConfigDB.chunks.updateMany(
        query, [{
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

function setHistoryWindowInSecs(st, valueInSeconds) {
    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: 'overrideHistoryWindowInSecs',
            mode: 'alwaysOn',
            data: {seconds: valueInSeconds}
        }));
    });
}

function resetHistoryWindowInSecs(st) {
    st.forEachConfigServer((conn) => {
        assert.commandWorked(
            conn.adminCommand({configureFailPoint: 'overrideHistoryWindowInSecs', mode: 'off'}));
    });
}

let defaultAutoMergerThrottlingMS = null;
function setBalancerMergeThrottling(st, valueInMS) {
    st.forEachConfigServer((conn) => {
        const res = conn.adminCommand({setParameter: 1, autoMergerThrottlingMS: valueInMS});
        assert.commandWorked(res);
        defaultAutoMergerThrottlingMS = res.was;
    });
}

function resetBalancerMergeThrottling(st) {
    if (!defaultAutoMergerThrottlingMS) {
        // Default throttling param was never changed, hence no need to reset it
        return;
    }

    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand(
            {setParameter: 1, autoMergerThrottlingMS: defaultAutoMergerThrottlingMS}));
    });
}

function setBalanceRoundInterval(st, valueInMs) {
    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: 'overrideBalanceRoundInterval',
            mode: 'alwaysOn',
            data: {intervalMs: valueInMs}
        }));
    });
}

function resetBalanceRoundInterval(st) {
    st.forEachConfigServer((conn) => {
        assert.commandWorked(
            conn.adminCommand({configureFailPoint: 'overrideBalanceRoundInterval', mode: 'off'}));
    });
}

function assertExpectedChunksOnShard(configDB, coll, shardName, expectedChunks) {
    const chunks =
        findChunksUtil.findChunksByNs(configDB, coll.getFullName(), {shard: shardName}).toArray();
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

    assertExpectedChunksOnShard(configDB, coll, shard0, expectedChunksPerShard[shard0]);
    assertExpectedChunksOnShard(configDB, coll, shard1, expectedChunksPerShard[shard1]);

    return findChunksUtil.findOneChunkByNs(configDB, coll.getFullName()).onCurrentShardSince;
}

/* Tests mergeAllChunks command */
function mergeAllChunksOnShardTest(st, testDB) {
    jsTestLog("Testing mergeAllChunksOnShard command");

    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInSecs(st, historyWindowInSeconds);

    // Distribute deterministically the chunks across the shards
    const now = buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds);

    // Make sure that all chunks are out of the history window
    setOnCurrentShardSince(st.s, coll, {}, now, -historyWindowInSeconds - 1000);

    // Merge all mergeable chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));
    assertExpectedChunksOnShard(
        configDB, coll, shard0, [{min: MinKey, max: 1}, {min: 3, max: 7}, {min: 10, max: MaxKey}]);

    // Merge all mergeable chunks on shard1
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunksOnShard(configDB, coll, shard1, [{min: 1, max: 3}, {min: 7, max: 10}]);
}

function mergeAllChunksWithMaxNumberOfChunksTest(st, testDB) {
    // Skip this test if running in a suite with stepdowns
    if (typeof ContinuousStepdown !== 'undefined') {
        return;
    }

    // Consider all chunks mergeable
    setHistoryWindowInSecs(st, -10 /* seconds */);

    const coll = newShardedColl(st, testDB);

    // Split unique chunk in 11 chunks on the same shard
    for (var i = 0; i < 10; i++) {
        splitChunk(st, coll, i /* middle */);
    }

    // Verify the `maxNumberOfChunksToMerge` is honored
    for (var i = 10; i > 0; i--) {
        assert.commandWorked(st.s.adminCommand({
            mergeAllChunksOnShard: coll.getFullName(),
            shard: st.shard0.shardName,
            maxNumberOfChunksToMerge: NumberInt(2)
        }));
        assert.eq(findChunksUtil.findChunksByNs(st.config, coll.getFullName()).toArray().length, i);
    }
}

/* Tests mergeAllChunks command considering history window preservation */
function mergeAllChunksOnShardConsideringHistoryWindowTest(st, testDB) {
    jsTestLog("Testing mergeAllChunksOnShard command considering history window preservation");

    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history windows to 1 min
    const historyWindowInSeconds = 1000;
    setHistoryWindowInSecs(st, 1000);

    // Distribute deterministically the chunks across the shards
    const now = buildInitialScenario(st, coll, shard0, shard1);

    // Initially, make all chunks older than history window
    setOnCurrentShardSince(st.s, coll, {}, now, -historyWindowInSeconds - 1000);

    // Perform some move so that those chunks will fall inside the history window and won't be able
    // to be merged
    moveRange(st, coll, 1, 2, shard0);
    moveRange(st, coll, 2, 3, shard0);

    // Try to merge all mergeable chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks must be merged except{min: 1, max: 2} and{min: 2, max: 3} because they
    // must be preserved when history widow is still active on them
    assertExpectedChunksOnShard(configDB, coll, shard0, [
        {min: MinKey, max: 1},
        {min: 1, max: 2},
        {min: 2, max: 3},
        {min: 3, max: 7},
        {min: 10, max: MaxKey}
    ]);

    // Try to merge all mergeable chunks on shard1 and check expected results
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunksOnShard(configDB, coll, shard1, [{min: 7, max: 10}]);
}

/* Tests mergeAllChunks command considering jumbo flag */
function mergeAllChunksOnShardConsideringJumboFlagTest(st, testDB) {
    jsTestLog("Testing mergeAllChunksOnShard command considering jumbo flag");

    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    const coll = newShardedColl(st, testDB);

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInSecs(st, historyWindowInSeconds);

    // Distribute deterministically the chunks across the shards
    const now = buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds);

    // Make sure that all chunks are out of the history window
    setOnCurrentShardSince(st.s, coll, {}, now, -historyWindowInSeconds - 1000);

    // Set jumbo flag to a couple of chunks
    // Setting a chunks as jumbo must prevent it from being merged
    setJumboFlag(configDB, coll, {min: {x: 3}});
    setJumboFlag(configDB, coll, {min: {x: 8}});

    // Try to merge all mergeable chunks on shard0
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks should be merged except {min: 3, max: 4}
    assertExpectedChunksOnShard(
        configDB,
        coll,
        shard0,
        [{min: MinKey, max: 1}, {min: 3, max: 4}, {min: 4, max: 7}, {min: 10, max: MaxKey}]);

    // Try to merge all mergeable chunks on shard1
    assert.commandWorked(
        st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));

    // All chunks should be merged except {min: 8, max: 9}
    assertExpectedChunksOnShard(
        configDB,
        coll,
        shard1,
        [{min: 1, max: 3}, {min: 7, max: 8}, {min: 8, max: 9}, {min: 9, max: 10}]);
}

/* Tests automerger on first balancer round */
function balancerTriggersAutomergerWhenIsEnabledTest(st, testDB) {
    jsTestLog("Testing automerger on first balancer round");

    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    // Create 3 collections
    const numColls = 3;
    let colls = [];
    for (let i = 0; i < numColls; i++) {
        colls.push(newShardedColl(st, testDB));
    }

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInSecs(st, historyWindowInSeconds);

    colls.forEach((coll) => {
        // Distribute deterministically the chunks across the shards
        const now = buildInitialScenario(st, coll, shard0, shard1, historyWindowInSeconds);

        // Make sure that all chunks are out of the history window
        setOnCurrentShardSince(st.s, coll, {}, now, -historyWindowInSeconds - 1000);
    });

    // Override balancer round interval and merge throttling to speed up the test
    setBalanceRoundInterval(st, 100 /* ms */);
    setBalancerMergeThrottling(st, 0);

    // Enable Automerger
    st.startBalancer();

    // Perform a couple of balancer rounds to give automerger time to do its job
    for (let i = 0; i < 3; ++i) {
        st.awaitBalancerRound();
    }

    // All mergeable chunks should be merged
    colls.forEach((coll) => {
        assertExpectedChunksOnShard(
            configDB,
            coll,
            shard0,
            [{min: MinKey, max: 1}, {min: 3, max: 7}, {min: 10, max: MaxKey}]);
        assertExpectedChunksOnShard(configDB, coll, shard1, [{min: 1, max: 3}, {min: 7, max: 10}]);
    });
}

function testConfigurableAutoMergerIntervalSecs(st, testDB) {
    // Override default configuration to speed up the test
    setBalanceRoundInterval(st, 100 /* ms */);
    setBalancerMergeThrottling(st, 0);
    setHistoryWindowInSecs(st, 0 /* seconds */);

    // Set automerger interval to 1 second
    st.forEachConfigServer((conn) => {
        assert.commandWorked(conn.adminCommand({setParameter: 1, autoMergerIntervalSecs: 1}));
    });

    st.startBalancer();
    // Potentially join previous balancing round with longer round interval from previous test case
    st.awaitBalancerRound();

    const coll = newShardedColl(st, testDB);

    // Repeatedly split the only chunk and expect the auto-merger to merge it back right away
    for (var i = 0; i < 3; i++) {
        assert.soonNoExcept(() => {
            // Split may fail if mongos doesn't see the previous merge (SERVER-54979)
            splitChunk(st, coll, 0 /* middle */);
            return true;
        });
        assert.soon(
            () =>
                findChunksUtil.findChunksByNs(st.config, coll.getFullName()).toArray().length == 1,
            "Automerger unexpectly didn't merge back chunks within a reasonable time");
    }
}

/* Test setup */
const st = new ShardingTest({mongos: 1, shards: 2, other: {enableBalancer: false}});

function executeTestCase(testFunc) {
    // Create database with `shard0` as primary shard
    const testDB = st.s.getDB(jsTestName());
    assert.commandWorked(
        st.s.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
    testFunc(st, testDB);

    // Teardown: stop the balancer, reset configuration and drop db
    st.stopBalancer();
    resetHistoryWindowInSecs(st);
    resetBalanceRoundInterval(st);
    resetBalancerMergeThrottling(st);

    // The auto-merger considers all collections, drop db so that collections from previous test
    // cases do not interfere
    testDB.dropDatabase();
}

executeTestCase(mergeAllChunksOnShardTest);
executeTestCase(mergeAllChunksWithMaxNumberOfChunksTest);
executeTestCase(mergeAllChunksOnShardConsideringHistoryWindowTest);
executeTestCase(mergeAllChunksOnShardConsideringJumboFlagTest);
executeTestCase(balancerTriggersAutomergerWhenIsEnabledTest);
executeTestCase(testConfigurableAutoMergerIntervalSecs);

st.stop();
})();
