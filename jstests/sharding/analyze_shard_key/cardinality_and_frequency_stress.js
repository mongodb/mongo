/**
 * Tests that the analyzeShardKey command still works correctly when the shard key values exceeds
 * the memory limit for aggregation.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const kSize100kB = 100 * 1024;

const numNodesPerRS = 2;
const numMostCommonValues = 5;
const internalDocumentSourceGroupMaxMemoryBytes = 1024 * 1024;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

/**
 * Finds the profiler entries for all aggregate and count commands with the given comment on the
 * given mongods and verifies that they all used index scan and did not fetch any documents, and
 * that they spilled to disk.
 */
function assertAggregationPlan(mongodConns, dbName, collName, isShardedColl, comment) {
    mongodConns.forEach(conn => {
        const profilerColl = conn.getDB(dbName).system.profile;
        profilerColl.find({"command.aggregate": collName, "command.comment": comment})
            .forEach(doc => {
                if (doc.hasOwnProperty("ok") && (doc.ok === 0)) {
                    return;
                }

                const firstStage = doc.command.pipeline[0];

                if (firstStage.hasOwnProperty("$collStats")) {
                    return;
                }

                if (isShardedColl) {
                    if (firstStage.hasOwnProperty("$mergeCursors")) {
                        // The profiler output for $mergeCursors aggregate commands is not expected
                        // to have a "planSummary" field.
                        assert(doc.usedDisk, doc);
                    } else {
                        assert(doc.hasOwnProperty("planSummary"), doc);
                        assert(doc.planSummary.includes("IXSCAN"), doc);
                        assert(!doc.usedDisk, doc);
                    }
                } else {
                    assert(doc.hasOwnProperty("planSummary"), doc);
                    assert(doc.planSummary.includes("IXSCAN"), doc);
                    assert(doc.usedDisk, doc);
                }

                // Verify that it did not fetch any documents.
                assert.eq(doc.docsExamined, 0, doc);
                // Verify that it opted out of shard filtering.
                assert.eq(doc.readConcern.level, "available", doc);
            });
    });
}

function testAnalyzeShardKeysUnshardedCollection(conn, mongodConns) {
    const dbName = "testDb";
    const collName = "testCollUnsharded";
    const ns = dbName + "." + collName;
    const candidateShardKey = {a: 1};
    const numDocs = 15;  // ~1.5MB in total.
    // Used to identify the operations performed by the analyzeShardKey commands in this test case.
    const comment = UUID();

    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    jsTest.log(
        `Testing analyzing a shard key for an unsharded collection: ${tojson({dbName, collName})}`);

    assert.commandWorked(coll.createIndex(candidateShardKey));

    const docs = [];
    const mostCommonValues = [];
    for (let i = 1; i <= numDocs; i++) {
        const chars = i.toString();
        const doc = {a: new Array(kSize100kB / chars.length).join(chars)};

        docs.push(doc);
        mostCommonValues.push({
            value: AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc, candidateShardKey),
            frequency: 1
        });
    }
    assert.commandWorked(coll.insert(docs, {writeConcern}));

    AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

    const res = assert.commandWorked(
        conn.adminCommand({analyzeShardKey: ns, key: candidateShardKey, comment}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res, {
        numDocs,
        isUnique: false,
        numDistinctValues: numDocs,
        mostCommonValues,
        numMostCommonValues
    });

    AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
    assertAggregationPlan(mongodConns, dbName, collName, false /* isShardedColl */, comment);

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeShardKeysShardedCollection(st, mongodConns) {
    const dbName = "testDb";
    const collName = "testCollSharded";
    const ns = dbName + "." + collName;
    const currentShardKey = {skey: 1};
    const currentShardKeySplitPoint = {skey: 0};
    const candidateShardKey = {a: 1};
    const numDocs = 30;  // ~1.5MB per shard.
    // Used to identify the operations performed by the analyzeShardKey commands in this test case.
    const comment = UUID();

    const db = st.s.getDB(dbName);
    const coll = db.getCollection(collName);

    jsTest.log(
        `Testing analyzing a shard key for a sharded collection: ${tojson({dbName, collName})}`);

    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: currentShardKey}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: currentShardKeySplitPoint}));
    assert.commandWorked(st.s.adminCommand(
        {moveChunk: ns, find: currentShardKeySplitPoint, to: st.shard1.shardName}));
    assert.commandWorked(coll.createIndex(candidateShardKey));

    const docs = [];
    const mostCommonValues = [];
    let sign = 1;
    for (let i = 1; i <= numDocs; i++) {
        const chars = i.toString();
        const doc = {a: new Array(kSize100kB / chars.length).join(chars), skey: sign};

        docs.push(doc);
        mostCommonValues.push({
            value: AnalyzeShardKeyUtil.extractShardKeyValueFromDocument(doc, candidateShardKey),
            frequency: 1
        });

        sign *= -1;
    }
    assert.commandWorked(coll.insert(docs, {writeConcern}));

    AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

    const res = assert.commandWorked(
        st.s.adminCommand({analyzeShardKey: ns, key: candidateShardKey, comment}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res, {
        numDocs,
        isUnique: false,
        numDistinctValues: numDocs,
        mostCommonValues,
        numMostCommonValues
    });

    AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
    assertAggregationPlan(mongodConns, dbName, collName, true /* isShardedColl */, comment);

    assert.commandWorked(db.dropDatabase());
}

const setParameterOpts = {
    internalDocumentSourceGroupMaxMemoryBytes,
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
    // Skip calculating the read and write distribution metrics since there are no sampled queries
    // anyway.
    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
        tojson({mode: "alwaysOn"}),
};

{
    const st = new ShardingTest({
        shards: numNodesPerRS,
        rs: {nodes: 2},
        other: {rsOptions: {setParameter: setParameterOpts}}
    });
    const mongodConns = [];
    st.rs0.nodes.forEach(node => mongodConns.push(node));
    st.rs1.nodes.forEach(node => mongodConns.push(node));

    testAnalyzeShardKeysUnshardedCollection(st.s, mongodConns);
    testAnalyzeShardKeysShardedCollection(st, mongodConns);

    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const mongodConns = rst.nodes;

    testAnalyzeShardKeysUnshardedCollection(rst.getPrimary(), mongodConns);

    rst.stopSet();
}
})();
