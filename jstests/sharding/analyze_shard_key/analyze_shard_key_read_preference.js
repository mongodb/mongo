/**
 * Tests that the analyzeShardKey command respects the read preference specified by the client.
 *
 * TODO (SERVER-74568): SdamServerSelector sometimes doesn't fully respect client readPreference for
 * config shard.
 * @tags: [requires_fcv_70, catalog_shard_incompatible]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // For 'extractUUIDFromObject'.
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const numShards = 2;
const analyzeShardKeyNumMostCommonValues = 5;
const analyzeShardKeyNumRanges = 10;
const st = new ShardingTest({
    mongos: 1,
    shards: numShards,
    rs: {
        nodes: [
            {rsConfig: {priority: 1}},
            {rsConfig: {priority: 0}},
            {rsConfig: {priority: 0, tags: {tag: "analytics"}}}
        ],
        setParameter: {analyzeShardKeyNumMostCommonValues, analyzeShardKeyNumRanges}
    }
});

/**
 * Sets up a sharded collection with the following chunks:
 * - shard0: [{x: MinKey}, {x: 0}]
 * - shard1: [{x: 0}, {x: MaxKey}]
 * such that the shard key {x: 1} have cardinality at least equal to 'analyzeShardKeyNumRanges'.
 */
function setUpCollection() {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

    const db = st.getDB(dbName);
    const coll = db.getCollection(collName);
    const collUuid = QuerySamplingUtil.getCollectionUuid(db, collName);

    // The sampling-based initial split policy needs 10 samples per split point so
    // 10 * analyzeShardKeyNumRanges is the minimum number of distinct shard key values that the
    // collection must have for the command to not fail to generate split points.
    const numDocs = 10 * analyzeShardKeyNumRanges;
    const docs = [];
    for (let i = 1; i <= numDocs / 2; i++) {
        docs.push({x: -i});
        docs.push({x: i});
    }
    // Waiting for the documents to get replicated to all nodes is necessary since the test later
    // runs the analyzeShardKey commands on secondaries.
    assert.commandWorked(coll.insert(docs, {writeConcern: {w: 3}}));

    return {dbName, collName, collUuid};
}

/**
 * Finds the profiler entries for commands that correspond to the analyzeShardKey command for the
 * given collection and verify that they have the expected read preferences. Increments the total
 * numbers of entries in 'numProfilerEntries' with the numbers of entries found.
 */
function assertReadPreferenceBasedOnProfiling(
    node, dbName, collName, collUuid, comment, expectedReadPref, numProfilerEntries) {
    const ns = dbName + "." + collName;

    const analyzeShardKeyProfilerDocs =
        node.getDB(dbName)
            .system.profile.find({"command.analyzeShardKey": ns, "command.comment": comment})
            .toArray();
    for (let doc of analyzeShardKeyProfilerDocs) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }
    numProfilerEntries.numAnalyzeShardKey += analyzeShardKeyProfilerDocs.length;

    const aggregateProfilerDocs =
        node.getDB(dbName)
            .system.profile.find({"command.aggregate": collName, "command.comment": comment})
            .toArray();
    for (let doc of aggregateProfilerDocs) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }
    numProfilerEntries.numAggregate += aggregateProfilerDocs.length;

    const configAggregateProfilerDocs =
        node.getDB("config")
            .system.profile
            .find({
                "command.aggregate":
                    {$regex: "^analyzeShardKey\.splitPoints\." + extractUUIDFromObject(collUuid)}
            })
            .toArray();
    for (let doc of configAggregateProfilerDocs) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }
    numProfilerEntries.numConfigAggregate += configAggregateProfilerDocs.length;
}

{
    jsTest.log(
        `Test the analyzeShardKey command respects the readPreference specified by the client`);
    const {dbName, collName, collUuid} = setUpCollection();
    const ns = dbName + "." + collName;
    // Used to identify the commands performed by the analyzeShardKey command in this test case.
    const comment = UUID();
    const analyzeShardKeyCmdObj = {
        analyzeShardKey: ns,
        key: {x: 1},
        $readPreference: {mode: "secondary", tags: [{tag: "analytics"}]},
        comment
    };
    const expectedReadPref = analyzeShardKeyCmdObj.$readPreference;

    // Make the analyzeShardKey command and aggregate commands fail on all nodes without the
    // "analytics" tag.
    const fpName = "failCommand";
    const fpData = {
        failInternalCommands: true,
        failLocalClients: true,
        failCommands: ["analyzeShardKey"],
        errorCode: ErrorCodes.InternalError,
        namespace: ns
    };
    let fps = [
        configureFailPoint(st.rs0.nodes[0], fpName, fpData),
        configureFailPoint(st.rs0.nodes[1], fpName, fpData),
        configureFailPoint(st.rs1.nodes[0], fpName, fpData),
        configureFailPoint(st.rs1.nodes[1], fpName, fpData)
    ];

    // Turn on profiling on the "analytics" nodes to verify the readPreference used below.
    AnalyzeShardKeyUtil.enableProfiler([st.rs0.nodes[2]], dbName);
    AnalyzeShardKeyUtil.enableProfiler([st.rs0.nodes[2]], "config");
    AnalyzeShardKeyUtil.enableProfiler([st.rs1.nodes[2]], dbName);
    AnalyzeShardKeyUtil.enableProfiler([st.rs1.nodes[2]], "config");

    // Run the analyzeShardKey command. If the specified readPreference is not respected, the
    // command would fail due to the fail points above.
    assert.commandWorked(st.s.adminCommand(analyzeShardKeyCmdObj));

    // Verify that the readPreference is as expected.
    let numProfilerEntries = {numAnalyzeShardKey: 0, numAggregate: 0, numConfigAggregate: 0};
    assertReadPreferenceBasedOnProfiling(
        st.rs0.nodes[2], dbName, collName, collUuid, comment, expectedReadPref, numProfilerEntries);
    assertReadPreferenceBasedOnProfiling(
        st.rs1.nodes[2], dbName, collName, collUuid, comment, expectedReadPref, numProfilerEntries);
    assert.gt(numProfilerEntries.numAnalyzeShardKey, 0, numProfilerEntries);
    assert.gt(numProfilerEntries.numAggregate, 0, numProfilerEntries);
    assert.gte(numProfilerEntries.numConfigAggregate, numShards, numProfilerEntries);

    // Turn off profiling.
    AnalyzeShardKeyUtil.disableProfiler([st.rs0.nodes[2]], dbName);
    AnalyzeShardKeyUtil.disableProfiler([st.rs0.nodes[2]], "config");
    AnalyzeShardKeyUtil.disableProfiler([st.rs1.nodes[2]], dbName);
    AnalyzeShardKeyUtil.disableProfiler([st.rs1.nodes[2]], "config");

    fps.forEach(fp => fp.off());
}

{
    jsTest.log(
        `Test the analyzeShardKey command uses readPreference "secondaryPreferred" by default`);
    const {dbName, collName, collUuid} = setUpCollection();
    const ns = dbName + "." + collName;
    // Used to identify the commands performed by the analyzeShardKey command in this test case.
    const comment = UUID();
    const analyzeShardKeyCmdObj = {analyzeShardKey: ns, key: {x: 1}, comment};
    const expectedReadPref = {mode: "secondaryPreferred"};

    // Turn on profiling on all nodes (since with "secondaryPreferred", any node can be targeted) to
    // verify the readPreference used below.
    AnalyzeShardKeyUtil.enableProfiler(st.rs0.nodes, dbName);
    AnalyzeShardKeyUtil.enableProfiler(st.rs0.nodes, "config");
    AnalyzeShardKeyUtil.enableProfiler(st.rs1.nodes, dbName);
    AnalyzeShardKeyUtil.enableProfiler(st.rs1.nodes, "config");

    assert.commandWorked(st.s.adminCommand(analyzeShardKeyCmdObj));

    // Verify that the readPreference is as expected.
    let numProfilerEntries = {numAnalyzeShardKey: 0, numAggregate: 0, numConfigAggregate: 0};
    st.rs0.nodes.forEach(node => {
        assertReadPreferenceBasedOnProfiling(
            node, dbName, collName, collUuid, comment, expectedReadPref, numProfilerEntries);
    });
    st.rs1.nodes.forEach(node => {
        assertReadPreferenceBasedOnProfiling(
            node, dbName, collName, collUuid, comment, expectedReadPref, numProfilerEntries);
    });
    assert.gt(numProfilerEntries.numAnalyzeShardKey, 0, numProfilerEntries);
    assert.gt(numProfilerEntries.numAggregate, 0, numProfilerEntries);
    assert.gte(numProfilerEntries.numConfigAggregate, numShards, numProfilerEntries);

    // Turn off profiling.
    AnalyzeShardKeyUtil.disableProfiler(st.rs0.nodes, dbName);
    AnalyzeShardKeyUtil.disableProfiler(st.rs0.nodes, "config");
    AnalyzeShardKeyUtil.disableProfiler(st.rs1.nodes, dbName);
    AnalyzeShardKeyUtil.disableProfiler(st.rs1.nodes, "config");
}

st.stop();
})();
