/**
 * Tests change stream opening parameters that can impact or get impacted by the resume
 * token version in combination with few other server settings. Change stream options:
 *  - startAtOperationTime, resumeAfter, startAfter - indicate the change stream's starting point
 * Change stream stages:
 *  - $changeStreamSplitLargeEvent - changes resume token version to V2
 * Aggregation options:
 *  - $_generateV2ResumeTokens - when 'true', changes resume token version to V2
 *  - $_passthroughToShard - allows shard access through mongoS connection
 * Server parameters and settings:
 *  - FCV (feature compatibility version)
 *  - enableTestCommands - activates test-only server features
 *
 * @tags: [
 *   uses_change_streams,
 *   requires_sharding,
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/change_stream_util.js");  // For ChangeStreamTest.

const dbName = "test";
const collName = jsTestName();

/**
 * Returns 4 resume tokens: 2 event tokens and 2 high-watermark tokens for resume token versions V1
 * and V2.
 */
function getDifferentResumeTokens(coll, startAtOperationTime) {
    const cst = new ChangeStreamTest(coll.getDB());
    const csCursorV1 = cst.startWatchingChanges({
        pipeline: [{"$changeStream": {startAtOperationTime}}],
        collection: coll,
    });

    // Use '$changeStreamSplitLargeEvent' stage to force V2 resume tokens, because it works with
    // either value of the 'enableTestCommands' parameter.
    const csCursorV2 = cst.startWatchingChanges({
        pipeline: [{"$changeStream": {startAtOperationTime}}, {$changeStreamSplitLargeEvent: {}}],
        collection: coll,
    });
    const result = [
        csCursorV1.postBatchResumeToken,
        csCursorV2.postBatchResumeToken,
        cst.getNextChanges(csCursorV1, 1)[0]._id,
        cst.getNextChanges(csCursorV2, 1)[0]._id
    ];
    cst.cleanUp();
    return result;
}

/**
 * Tests that a change stream cursor can be created with the given parameters, and the subsequent
 * 'getMore' command succeeds, unless '$_generateV2ResumeTokens' is true when 'enableTestCommands'
 * is false.
 */
function assertChangeStreamOpensAndWorks(
    conn, enableTestCommands, fcv, csPipeline, csOptions, aggregateOptions) {
    jsTestLog("Testing combination " +
              tojson({conn, enableTestCommands, fcv, csPipeline, csOptions, aggregateOptions},
                     false,
                     true));
    try {
        const cst = new ChangeStreamTest(conn.getDB(dbName));

        // Ensure that 'aggregate' and 'getMore' commands are issued on the server.
        const csCursor = cst.startWatchingChanges({
            pipeline: [{"$changeStream": csOptions}].concat(csPipeline),
            collection: collName,
            aggregateOptions
        });
        cst.getNextBatch(csCursor);

        // Close unneeded cursors.
        cst.cleanUp();
    } catch (e) {
        // Accept the errors explicitly prohibiting '$_generateV2ResumeTokens' without
        // 'enableTestCommands'.
        if (!enableTestCommands && aggregateOptions["$_generateV2ResumeTokens"] &&
            (e.toString().includes("Location6528200") ||
             e.toString().includes("Location6528201"))) {
            return;
        }
        throw e;
    }
}

function testChangeStreamOpeningWithAllCombinationsOfOptions(connections, enableTestCommands) {
    // If there is more than one connection, expect the additional connections to be shard
    // connections.
    const [conn, ...shardConnections] = connections;
    const testDB = conn.getDB(dbName);
    const testColl = testDB[collName];

    // Capture cluster time before any events.
    const startClusterTime = testDB.hello().$clusterTime.clusterTime;

    // Create an event to obtain valid event resume tokens. Create a second event to speed-up the
    // response time of 'getMore' commands.
    testColl.insertOne({_id: 123});
    testColl.insertOne({_id: 456});

    // Create test dimension for change stream start options. Change streams support 3 mutually
    // exclusive 'start' parameters: 'resumeAfter', 'startAfter', and 'startAtOperationTime'. All
    // these parameters are optional, so the 4th case is when none is specified. We combine the use
    // of 'resumeAfter' and 'startAfter' with every type and version of resume tokens.
    const changeStreamStartOptions =
        getDifferentResumeTokens(testColl, startClusterTime)
            .reduce((result, token) => result.concat([{resumeAfter: token}, {startAfter: token}]),
                    [{}, {startAtOperationTime: startClusterTime}]);

    // In case of a sharded cluster, exercise the '$_passthroughToShard' option for each shard.
    const passthroughToShardOptions = [{}].concat(
        shardConnections.map(shardConn => ({$_passthroughToShard: {shard: shardConn.shardName}})));

    for (const fcv of [latestFCV, lastLTSFCV]) {
        assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: fcv}));
        for (const conn of connections) {
            for (const pipeline of [[], [{$changeStreamSplitLargeEvent: {}}]]) {
                for (const passthroughToShard of passthroughToShardOptions) {
                    for (const generateV2ResumeTokens of [{},
                                                          {$_generateV2ResumeTokens: false},
                                                          {$_generateV2ResumeTokens: true}]) {
                        for (const changeStreamStart of changeStreamStartOptions) {
                            assertChangeStreamOpensAndWorks(
                                conn,
                                enableTestCommands,
                                fcv,
                                pipeline,
                                changeStreamStart,
                                Object.assign({}, passthroughToShard, generateV2ResumeTokens));
                        }
                    }
                }
            }
        }
    }
}

for (const enableTestCommands of [false, true]) {
    TestData.enableTestCommands = enableTestCommands;

    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    testChangeStreamOpeningWithAllCombinationsOfOptions([st.s, st.shard0], enableTestCommands);
    st.stop();

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    testChangeStreamOpeningWithAllCombinationsOfOptions([rst.getPrimary()], enableTestCommands);
    rst.stopSet();
}
})();
