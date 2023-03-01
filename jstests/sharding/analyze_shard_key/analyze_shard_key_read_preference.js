/**
 * Tests that the analyzeShardKey command respects the read preference specified by the client.
 *
 * @tags: [requires_fcv_70, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // For 'extractUUIDFromObject'.
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");
load("jstests/sharding/analyze_shard_key/libs/query_sampling_util.js");

const analyzeShardKeyNumMostCommonValues = 5;
const analyzeShardKeyNumRanges = 10;
const st = new ShardingTest({
    mongos: 1,
    shards: 2,
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
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard0.shardName}));

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
 * Asserts that the analyzeShardKey command for the given collection used the expected read
 * preference.
 */
function assertReadPreference(node, dbName, collName, collUuid, expectedReadPref) {
    const ns = dbName + "." + collName;

    const analyzeShardKeyProfilerDocs =
        node.getDB(dbName).system.profile.find({"command.analyzeShardKey": ns}).toArray();
    for (let doc of analyzeShardKeyProfilerDocs) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }

    const aggregateProfilerDocs =
        node.getDB(dbName).system.profile.find({"command.aggregate": collName}).toArray();
    for (let doc of aggregateProfilerDocs) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }

    const aggregateProfilerDocsConfig =
        node.getDB("config")
            .system.profile
            .find({
                "command.aggregate":
                    {$regex: "^analyzeShardKey\.splitPoints\." + extractUUIDFromObject(collUuid)}
            })
            .toArray();
    for (let doc of aggregateProfilerDocsConfig) {
        assert.eq(0,
                  bsonWoCompare(doc.command.$readPreference, expectedReadPref),
                  {actual: doc.command.$readPreference, expected: expectedReadPref});
    }
}

{
    jsTest.log(
        `Test the analyzeShardKey command respects the readPreference specified by the client`);
    const {dbName, collName, collUuid} = setUpCollection();
    const ns = dbName + "." + collName;
    const analyzeShardKeyCmdObj = {
        analyzeShardKey: ns,
        key: {x: 1},
        $readPreference: {mode: "secondaryPreferred", tags: [{tag: "analytics"}]}
    };
    const expectedReadPref = analyzeShardKeyCmdObj.$readPreference;

    // Make the analyzeShardKey command and aggregate commands fail on all nodes without the
    // "analytics" tag.
    const fpName = "failCommand";
    const fpData = {
        failInternalCommands: true,
        failLocalClients: true,
        failCommands: ["analyzeShardKey", "aggregate"],
        errorCode: ErrorCodes.InternalError,
        ns
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

    // Run the analyzeShardKey command. If the specified readPreference is not respected in all
    // steps of metrics calculation, the command would fail due to the fail points above.
    assert.commandWorked(st.s.adminCommand(analyzeShardKeyCmdObj));

    // Verify that the readPreference is as expected.
    assertReadPreference(st.rs0.nodes[2], dbName, collName, collUuid, expectedReadPref);
    assertReadPreference(st.rs0.nodes[2], dbName, collName, collUuid, expectedReadPref);

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
    const analyzeShardKeyCmdObj = {analyzeShardKey: ns, key: {x: 1}};
    const expectedReadPref = {mode: "secondaryPreferred"};

    // Turn on profiling on all nodes (since with "secondaryPreferred", any node can be targeted) to
    // verify the readPreference used below.
    AnalyzeShardKeyUtil.enableProfiler(st.rs0.nodes, dbName);
    AnalyzeShardKeyUtil.enableProfiler(st.rs0.nodes, "config");
    AnalyzeShardKeyUtil.enableProfiler(st.rs1.nodes, dbName);
    AnalyzeShardKeyUtil.enableProfiler(st.rs1.nodes, "config");

    assert.commandWorked(st.s.adminCommand(analyzeShardKeyCmdObj));

    // Verify that the readPreference is as expected.
    st.rs0.nodes.forEach(node => {
        assertReadPreference(node, dbName, collName, collUuid, expectedReadPref);
    });
    st.rs1.nodes.forEach(node => {
        assertReadPreference(node, dbName, collName, collUuid, expectedReadPref);
    });

    // Turn off profiling.
    AnalyzeShardKeyUtil.disableProfiler(st.rs0.nodes, dbName);
    AnalyzeShardKeyUtil.disableProfiler(st.rs0.nodes, "config");
    AnalyzeShardKeyUtil.disableProfiler(st.rs1.nodes, dbName);
    AnalyzeShardKeyUtil.disableProfiler(st.rs1.nodes, "config");
}

st.stop();
})();
