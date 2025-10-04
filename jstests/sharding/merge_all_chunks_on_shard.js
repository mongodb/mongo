/**
 * Tests mergeAllChunksOnShard command and auto-merger behavior
 *
 * @tags: [
 *   requires_fcv_70,
 * ]
 */

import {configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

/* Create new sharded collection on testDB */
let _collCounter = 0;
function newShardedColl(st, testDB) {
    const collNamePrefix = "coll";
    const coll = testDB[collNamePrefix + "_" + _collCounter++];
    assert.commandWorked(st.s.adminCommand({shardCollection: coll.getFullName(), key: {x: 1}}));
    return coll;
}

/* Split chunk */
function splitChunk(st, coll, splitPointKeyValue) {
    assert.commandWorked(st.s.adminCommand({split: coll.getFullName(), middle: {x: splitPointKeyValue}}));
}

/* Move range */
function moveRange(st, coll, minKeyValue, maxKeyValue, toShard) {
    assert.commandWorked(
        st.s.adminCommand({
            moveRange: coll.getFullName(),
            min: {x: minKeyValue},
            max: {x: maxKeyValue},
            toShard: toShard,
        }),
    );
}

/* Set `onCurrentShardSince` field to (refTimestamp + offsetInSeconds) */
function setOnCurrentShardSince(mongoS, coll, extraQuery, refTimestamp, offsetInSeconds) {
    // Use 'retryWrites' when writing to the configsvr because they are not automatically retried.
    const mongosSession = mongoS.startSession({retryWrites: true});
    const sessionConfigDB = mongosSession.getDatabase("config");
    const collUuid = sessionConfigDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, extraQuery);
    const newValue = new Timestamp(refTimestamp.getTime() + offsetInSeconds, 0);
    const chunks = sessionConfigDB.chunks.find(query);
    chunks.forEach((chunk) => {
        assert.commandWorked(
            sessionConfigDB.chunks.updateOne({_id: chunk._id}, [
                {
                    $set: {
                        "onCurrentShardSince": newValue,
                        "history": [{validAfter: newValue, shard: "$shard"}],
                    },
                },
            ]),
        );
    });
}

/* Set jumbo flag to true */
function setJumboFlag(mongoS, coll, chunkQuery) {
    // Use 'retryWrites' when writing to the configsvr because they are not automatically retried.
    const mongosSession = mongoS.startSession({retryWrites: true});
    const sessionConfigDB = mongosSession.getDatabase("config");
    const collUuid = sessionConfigDB.collections.findOne({_id: coll.getFullName()}).uuid;
    const query = Object.assign({uuid: collUuid}, chunkQuery);
    assert.commandWorked(sessionConfigDB.chunks.update(query, {$set: {jumbo: true}}));
}

function setHistoryWindowInSecs(st, valueInSeconds) {
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {seconds: valueInSeconds}, "alwaysOn");
}

function resetHistoryWindowInSecs(st) {
    configureFailPointForRS(st.configRS.nodes, "overrideHistoryWindowInSecs", {}, "off");
}

function updateBalancerParameter(st, paramName, valueInMS) {
    let prevParamValue = null;
    st.forEachConfigServer((conn) => {
        const res = conn.adminCommand({setParameter: 1, [paramName]: valueInMS});
        assert.commandWorked(res);
        prevParamValue = res.was;
    });
    return prevParamValue;
}

let defaultAutoMergerThrottlingMS = null;
function setBalancerMergeThrottling(st, valueInMS) {
    const prevValue = updateBalancerParameter(st, "autoMergerThrottlingMS", valueInMS);
    // Cache only the initial [default] parameter value
    if (!defaultAutoMergerThrottlingMS) defaultAutoMergerThrottlingMS = prevValue;
}

function resetBalancerMergeThrottling(st) {
    // No need to reset if parameter at initial state
    if (!defaultAutoMergerThrottlingMS) return;
    updateBalancerParameter(st, "autoMergerThrottlingMS", defaultAutoMergerThrottlingMS);
    defaultAutoMergerThrottlingMS = null;
}

let defaultBalancerMigrationsThrottlingMs = null;
function setBalancerMigrationsThrottling(st, valueInMS) {
    const prevValue = updateBalancerParameter(st, "balancerMigrationsThrottlingMs", valueInMS);
    // Cache only the initial [default] parameter value
    if (!defaultBalancerMigrationsThrottlingMs) defaultBalancerMigrationsThrottlingMs = prevValue;
}

function resetBalancerMigrationsThrottling(st) {
    // No need to reset if parameter at initial state
    if (!defaultBalancerMigrationsThrottlingMs) return;
    updateBalancerParameter(st, "balancerMigrationsThrottlingMs", defaultBalancerMigrationsThrottlingMs);
    defaultBalancerMigrationsThrottlingMs = null;
}

function assertExpectedChunks(configDB, coll, expectedChunksPerShard) {
    for (const [shardName, expectedChunks] of Object.entries(expectedChunksPerShard)) {
        const chunks = findChunksUtil.findChunksByNs(configDB, coll.getFullName(), {shard: shardName}).toArray();
        assert.eq(expectedChunks.length, chunks.length, chunks);
        expectedChunks.forEach((expectedChunk) => {
            const chunkFound = chunks.some(
                (chunk) => expectedChunk.min === chunk.min.x && expectedChunk.max === chunk.max.x,
            );
            assert(
                chunkFound,
                "Couldn't find expected range {min: " +
                    tojson(expectedChunk.min) +
                    ", max: " +
                    tojson(expectedChunk.max) +
                    "} on shard " +
                    tojson(shardName),
            );
        });
    }
}

function waitForAutomergerToCompleteAndAssert(configDB, coll, expectedChunksPerShard) {
    assert.soonNoExcept(() => {
        assertExpectedChunks(configDB, coll, expectedChunksPerShard);
        return true;
    }, "Automerger didn't merge the expected chunks within a reasonable time");
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
            {min: 10, max: MaxKey},
        ],
        [shard1]: [
            {min: 1, max: 2},
            {min: 2, max: 3},
            {min: 7, max: 8},
            {min: 8, max: 9},
            {min: 9, max: 10},
        ],
    };

    const configDB = st.getDB("config");

    assertExpectedChunks(configDB, coll, expectedChunksPerShard);

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
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));
    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 1},
            {min: 3, max: 7},
            {min: 10, max: MaxKey},
        ],
    });

    // Merge all mergeable chunks on shard1
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunks(configDB, coll, {
        [shard1]: [
            {min: 1, max: 3},
            {min: 7, max: 10},
        ],
    });
}

function mergeAllChunksWithMaxNumberOfChunksTest(st, testDB) {
    // Skip this test if running in a suite with stepdowns
    if (TestData.runningWithConfigStepdowns) {
        jsTest.log(`Skipping mergeAllChunksWithMaxNumberOfChunksTest`);
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
        assert.commandWorked(
            st.s.adminCommand({
                mergeAllChunksOnShard: coll.getFullName(),
                shard: st.shard0.shardName,
                maxNumberOfChunksToMerge: NumberInt(2),
            }),
        );
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
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks must be merged except{min: 1, max: 2} and{min: 2, max: 3} because they
    // must be preserved when history widow is still active on them
    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 1},
            {min: 1, max: 2},
            {min: 2, max: 3},
            {min: 3, max: 7},
            {min: 10, max: MaxKey},
        ],
    });

    // Try to merge all mergeable chunks on shard1 and check expected results
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));
    assertExpectedChunks(configDB, coll, {[shard1]: [{min: 7, max: 10}]});
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
    setJumboFlag(st.s, coll, {min: {x: 3}});
    setJumboFlag(st.s, coll, {min: {x: 8}});

    // Try to merge all mergeable chunks on shard0
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    // All chunks should be merged except {min: 3, max: 4}
    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 1},
            {min: 3, max: 4},
            {min: 4, max: 7},
            {min: 10, max: MaxKey},
        ],
    });

    // Try to merge all mergeable chunks on shard1
    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard1}));

    // All chunks should be merged except {min: 8, max: 9}
    assertExpectedChunks(configDB, coll, {
        [shard1]: [
            {min: 1, max: 3},
            {min: 7, max: 8},
            {min: 8, max: 9},
            {min: 9, max: 10},
        ],
    });
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

    // Update balancer migration/merge throttling to speed up the test
    setBalancerMigrationsThrottling(st, 100);
    setBalancerMergeThrottling(st, 0);

    // Enable the AutoMerger
    st.startBalancer();

    // All mergeable chunks should be eventually merged by the AutoMerger
    colls.forEach((coll) => {
        waitForAutomergerToCompleteAndAssert(configDB, coll, {
            [shard0]: [
                {min: MinKey, max: 1},
                {min: 3, max: 7},
                {min: 10, max: MaxKey},
            ],
        });

        waitForAutomergerToCompleteAndAssert(configDB, coll, {
            [shard1]: [
                {min: 1, max: 3},
                {min: 7, max: 10},
            ],
        });
    });

    st.stopBalancer();
}

function testConfigurableAutoMergerIntervalSecs(st, testDB) {
    // Override default configuration to speed up the test
    setBalancerMigrationsThrottling(st, 100);
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
    for (let i = 0; i < 3; i++) {
        assert.soonNoExcept(() => {
            // Split may fail if mongos doesn't see the previous merge (SERVER-54979)
            splitChunk(st, coll, 0 /* middle */);
            return true;
        });
        assert.soon(
            () => findChunksUtil.findChunksByNs(st.config, coll.getFullName()).toArray().length == 1,
            "Automerger unexpectly didn't merge back chunks within a reasonable time",
        );
    }
}

function testMaxTimeProcessingChunksMSParameter(st, testDB) {
    jsTestLog("Testing the `maxTimeProcessingChunksMS` parameter.");

    const configDB = st.s.getDB("config");
    const shard0 = st.shard0.shardName;
    const shard1 = st.shard1.shardName;

    /* Build the following initial scenario:
     *  - shard0
     *         { min: MinKey, max: 0 }
     *
     *         { min: 1,      max: 2 }
     *
     *         { min: 3,      max: 4 }
     *         { min: 4,      max: 5 }
     *         { min: 5,      max: 6 }
     *
     *         { min: 7,      max: 8 }
     *         { min: 8,      max: MaxKey }
     *
     *  - shard1
     *         { min: 0,      max: 1 }
     *
     *         { min: 2,      max: 3 }
     *
     *         { min: 6,      max: 7 }
     */
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName(), primaryShard: shard0}));
    const coll = newShardedColl(st, testDB);

    for (let i = 0; i <= 8; i++) {
        splitChunk(st, coll, i);
    }
    moveRange(st, coll, 0, 1, shard1);
    moveRange(st, coll, 2, 3, shard1);
    moveRange(st, coll, 6, 7, shard1);

    // Set history window to a known value
    const historyWindowInSeconds = 30;
    setHistoryWindowInSecs(st, historyWindowInSeconds);

    // Make sure that all chunks are out of the history window
    const now = findChunksUtil.findOneChunkByNs(configDB, coll.getFullName()).onCurrentShardSince;
    setOnCurrentShardSince(st.s, coll, {}, now, -historyWindowInSeconds - 1000);

    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 0},
            {min: 1, max: 2},
            {min: 3, max: 4},
            {min: 4, max: 5},
            {min: 5, max: 6},
            {min: 7, max: 8},
            {min: 8, max: MaxKey},
        ],
        [shard1]: [
            {min: 0, max: 1},
            {min: 2, max: 3},
            {min: 6, max: 7},
        ],
    });

    // Run mergeAllChunksOnShard on shard 'shard0' and forcing a timeout of the operation.
    const failpoint = configureFailPointForRS(st.configRS.nodes, "hangMergeAllChunksUntilReachingTimeout");

    assert.commandWorked(
        st.s.adminCommand({
            mergeAllChunksOnShard: coll.getFullName(),
            shard: shard0,
            maxTimeProcessingChunksMS: NumberInt(1),
        }),
    );

    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 0},
            {min: 1, max: 2},
            {min: 3, max: 6},
            {min: 7, max: 8},
            {min: 8, max: MaxKey},
        ],
        [shard1]: [
            {min: 0, max: 1},
            {min: 2, max: 3},
            {min: 6, max: 7},
        ],
    });

    failpoint.off();

    assert.commandWorked(st.s.adminCommand({mergeAllChunksOnShard: coll.getFullName(), shard: shard0}));

    assertExpectedChunks(configDB, coll, {
        [shard0]: [
            {min: MinKey, max: 0},
            {min: 1, max: 2},
            {min: 3, max: 6},
            {min: 7, max: MaxKey},
        ],
        [shard1]: [
            {min: 0, max: 1},
            {min: 2, max: 3},
            {min: 6, max: 7},
        ],
    });
}

/* Test setup */
const st = new ShardingTest({mongos: 1, shards: 2, other: {enableBalancer: false}});

function executeTestCase(testFunc) {
    // Create database with `shard0` as primary shard
    const testDB = st.s.getDB(jsTestName());
    assert.commandWorked(st.s.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));
    testFunc(st, testDB);

    // Teardown: stop the balancer, reset configuration and drop db
    st.stopBalancer();
    resetHistoryWindowInSecs(st);
    resetBalancerMigrationsThrottling(st);
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
executeTestCase(testMaxTimeProcessingChunksMSParameter);

st.stop();
