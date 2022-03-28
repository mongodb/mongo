/**
 * Tests whether $sum accumulator incorrect result bug is fixed when upgrading from the last-lts to
 * the latest.
 *
 * @tags: [
 *  requires_sharding,
 * ]
 */
(function() {
'use strict';

load('jstests/multiVersion/libs/multi_cluster.js');  // For upgradeCluster()

// TODO SERVER-64227 Remove this test case since this test case is unnecessary after we branch
// for 6.1.
(function testUpgradeFromLastLtsToLatest() {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2, binVersion: "last-lts"},
        other: {mongosOptions: {binVersion: "last-lts"}}
    });

    let db = st.getDB(jsTestName());
    let dbAtShard0 = st.shard0.getDB(jsTestName());
    let dbAtShard1 = st.shard1.getDB(jsTestName());

    // Makes sure that the test db is sharded.
    assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));

    let verifyShardedAccumulatorResultsOnBothEngine =
        (isGreaterLastContinous, coll, pipeline, verifyThis) => {
            // In the last-lts, we don't have the 'internalQueryForceClassicEngine' query knob.
            if (isGreaterLastContinous) {
                // Turns to the classic engine at the shards.
                assert.commandWorked(dbAtShard0.adminCommand(
                    {setParameter: 1, internalQueryForceClassicEngine: true}));
                assert.commandWorked(dbAtShard1.adminCommand(
                    {setParameter: 1, internalQueryForceClassicEngine: true}));
            }

            // Verifies that the classic engine's results are same as the expected results.
            const classicRes = coll.aggregate(pipeline).toArray();
            verifyThis(classicRes);

            // In the last-lts, we have neither the 'internalQueryForceClassicEngine' query knob
            // nor the SBE $group pushdown feature.
            if (isGreaterLastContinous) {
                // Turns to the SBE engine at the shards.
                assert.commandWorked(dbAtShard0.adminCommand(
                    {setParameter: 1, internalQueryForceClassicEngine: false}));
                assert.commandWorked(dbAtShard1.adminCommand(
                    {setParameter: 1, internalQueryForceClassicEngine: false}));

                // Verifies that the SBE engine's results are same as the expected results.
                const sbeRes = coll.aggregate(pipeline).toArray();
                verifyThis(sbeRes);
            }
        };

    let shardCollectionByHashing = coll => {
        coll.drop();

        // Makes sure that the collection is sharded.
        assert.commandWorked(
            st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

        return coll;
    };

    let hashShardedColl = shardCollectionByHashing(db.partial_sum);
    let unshardedColl = db.partial_sum2;

    for (let i = 0; i < 10; ++i) {
        const docs = [
            {k: i, n: 1e+34},
            {k: i, n: NumberDecimal("0.1")},
            {k: i, n: NumberDecimal("0.01")},
            {k: i, n: -1e+34}
        ];
        assert.commandWorked(hashShardedColl.insert(docs));
        assert.commandWorked(unshardedColl.insert(docs));
    }

    const pipelineWithSum = [{$group: {_id: "$k", s: {$sum: "$n"}}}, {$group: {_id: "$s"}}];
    const pipelineWithAvg = [{$group: {_id: "$k", s: {$avg: "$n"}}}, {$group: {_id: "$s"}}];

    // The results on an unsharded collection is the expected results.
    let expectedRes = unshardedColl.aggregate(pipelineWithSum).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        false /* isGreaterLastContinous */,
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.neq(
            actualRes,
            expectedRes,
            `Sharded sum for mixed data by which only decimal sum survive on ${version}`));

    expectedRes = unshardedColl.aggregate(pipelineWithAvg).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        false /* isGreaterLastContinous */,
        hashShardedColl,
        pipelineWithAvg,
        (actualRes) => assert.neq(
            actualRes,
            expectedRes,
            `Sharded avg for mixed data by which only decimal sum survive on ${version}`));

    // Upgrade the cluster to the latest.
    st.upgradeCluster(
        "latest",
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});

    db = st.getDB(jsTestName());
    checkFCV(st.rs0.getPrimary().getDB("admin"), lastLTSFCV);
    dbAtShard0 = st.shard0.getDB(jsTestName());
    dbAtShard1 = st.shard1.getDB(jsTestName());

    hashShardedColl = db.partial_sum;
    unshardedColl = db.partial_sum2;

    // $sum fix is FCV-gated. So, it's not applied after binary upgrade.
    expectedRes = unshardedColl.aggregate(pipelineWithSum).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        true /* isGreaterLastContinuous */,
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.neq(
            actualRes,
            expectedRes,
            "Sharded sum for mixed data by which only decimal sum survive on latest after binary upgrade"));

    // On the other hand, $avg fix is not FCV-gated. So, it's applied after binary upgrade.
    expectedRes = unshardedColl.aggregate(pipelineWithAvg).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        true /* isGreaterLastContinuous */,
        hashShardedColl,
        pipelineWithAvg,
        (actualRes) => assert.eq(
            actualRes,
            expectedRes,
            "Sharded avg for mixed data by which only decimal sum survive on latest after binary upgrade"));

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // The FCV is upgraded to the 'latestFCV' and $sum fix must be applied now.
    expectedRes = unshardedColl.aggregate(pipelineWithSum).toArray();
    verifyShardedAccumulatorResultsOnBothEngine(
        true /* isGreaterLastContinuous */,
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.eq(
            actualRes,
            expectedRes,
            "Sharded sum for mixed data by which only decimal sum survive on latest after FCV upgrade"));

    st.stop();
}());
}());
