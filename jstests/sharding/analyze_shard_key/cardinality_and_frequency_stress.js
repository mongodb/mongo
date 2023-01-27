/**
 * Tests that the analyzeShardKey command still works correctly when the shard key values exceeds
 * the memory limit for aggregation.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const kInternalDocumentSourceGroupMaxMemoryBytes = 1024 * 1024;
const kSize100kB = 100 * 1024;

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

                if (isShardedColl) {
                    const isMerge = doc.command.pipeline[0].hasOwnProperty("$mergeCursors");
                    if (isMerge) {
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
    const dbName = "testDbCandidateUnsharded";
    const collName = "testColl";
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
    for (let i = 1; i <= numDocs; i++) {
        const chars = i.toString();
        const doc = {a: new Array(kSize100kB / chars.length).join(chars)};
        assert.commandWorked(db.runCommand({insert: collName, documents: [doc]}));
    }

    AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

    const res = assert.commandWorked(
        conn.adminCommand({analyzeShardKey: ns, key: candidateShardKey, comment}));
    assert.eq(res.numDocs, numDocs, res);
    assert.eq(res.isUnique, false, res);
    assert.eq(res.cardinality, numDocs, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}), 0, res);

    AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
    assertAggregationPlan(mongodConns, dbName, collName, false /* isShardedColl */, comment);

    assert.commandWorked(db.dropDatabase());
}

function testAnalyzeShardKeysShardedCollection(st, mongodConns) {
    const dbName = "testDbCandidateSharded";
    const collName = "testColl";
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

    let sign = 1;
    for (let i = 1; i <= numDocs; i++) {
        const chars = i.toString();
        const doc = {a: new Array(kSize100kB / chars.length).join(chars), skey: sign};
        assert.commandWorked(db.runCommand({insert: collName, documents: [doc]}));
        sign *= -1;
    }

    AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

    const res = assert.commandWorked(
        st.s.adminCommand({analyzeShardKey: ns, key: candidateShardKey, comment}));
    assert.eq(res.numDocs, numDocs, res);
    assert.eq(res.isUnique, false, res);
    assert.eq(res.cardinality, numDocs, res);
    assert.eq(bsonWoCompare(res.frequency, {p99: 1, p95: 1, p90: 1, p80: 1, p50: 1}), 0, res);

    AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
    assertAggregationPlan(mongodConns, dbName, collName, true /* isShardedColl */, comment);

    assert.commandWorked(db.dropDatabase());
}

{
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2},
        other: {
            rsOptions: {
                setParameter: {
                    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
                        tojson({mode: "alwaysOn"}),
                    internalDocumentSourceGroupMaxMemoryBytes:
                        kInternalDocumentSourceGroupMaxMemoryBytes
                }
            }
        }
    });
    const mongodConns = [];
    st.rs0.nodes.forEach(node => mongodConns.push(node));
    st.rs1.nodes.forEach(node => mongodConns.push(node));

    testAnalyzeShardKeysUnshardedCollection(st.s, mongodConns);
    testAnalyzeShardKeysShardedCollection(st, mongodConns);

    st.stop();
}

{
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            setParameter: {
                internalDocumentSourceGroupMaxMemoryBytes:
                    kInternalDocumentSourceGroupMaxMemoryBytes
            }
        }
    });
    rst.startSet();
    rst.initiate();
    const mongodConns = rst.nodes;

    testAnalyzeShardKeysUnshardedCollection(rst.getPrimary(), mongodConns);

    rst.stopSet();
}
})();
