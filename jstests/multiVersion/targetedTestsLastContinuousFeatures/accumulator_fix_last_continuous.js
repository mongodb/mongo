/**
 * Tests whether $sum accumulator incorrect result bug is fixed when upgrading from the
 * last-continuous to the latest.
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
(function testUpgradeFromLastContinuousToLatest() {
    const st = new ShardingTest({
        shards: 2,
        rs: {nodes: 2, binVersion: "last-continuous"},
        other: {mongosOptions: {binVersion: "last-continuous"}}
    });

    let db = st.getDB(jsTestName());

    // Makes sure that the test db is sharded.
    assert.commandWorked(st.s0.adminCommand({enableSharding: db.getName()}));

    let verifyShardedAccumulatorResultsOnBothEngine = (coll, pipeline, verifyThis) => {
        const dbs = [
            st.rs0.getPrimary().getDB(jsTestName()),
            st.rs0.getSecondary().getDB(jsTestName()),
            st.rs1.getPrimary().getDB(jsTestName()),
            st.rs1.getSecondary().getDB(jsTestName())
        ];

        function setEngine(db, turnOnSBE) {
            // Based on which version we are running, set the appropriate parameter which
            // controls the execution engine.
            const res = db.adminCommand({
                getParameter: 1,
                internalQueryEnableSlotBasedExecutionEngine: 1,
                internalQueryForceClassicEngine: 1
            });

            if (res.hasOwnProperty("internalQueryEnableSlotBasedExecutionEngine")) {
                assert.commandWorked(
                    db.adminCommand(
                        {setParameter: 1, internalQueryEnableSlotBasedExecutionEngine: turnOnSBE}),
                    `at node ${db.getMongo().host}`);
            } else {
                assert(res.hasOwnProperty("internalQueryForceClassicEngine"));
                assert.commandWorked(
                    db.adminCommand({setParameter: 1, internalQueryForceClassicEngine: !turnOnSBE}),
                    `at node ${db.getMongo().host}`);
            }
        }

        // Turns to the classic engine at the shards.
        dbs.forEach((db) => setEngine(db, false /* turnOnSBE */));

        // Verifies that the classic engine's results are same as the expected results.
        const classicRes = coll.aggregate(pipeline).toArray();
        verifyThis(classicRes);

        // Turns to the SBE engine at the shards.
        dbs.forEach((db) => setEngine(db, true /* turnOnSBE */));

        // Verifies that the SBE engine's results are same as the expected results.
        const sbeRes = coll.aggregate(pipeline).toArray();
        verifyThis(sbeRes);
    };

    let shardCollectionByHashing = coll => {
        coll.drop();

        // Makes sure that the collection is sharded.
        assert.commandWorked(
            st.s0.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}));

        return coll;
    };

    let hashShardedColl = shardCollectionByHashing(db.partial_sum);

    for (let i = 0; i < 10; ++i) {
        // We set predetermined values for _id so that our data can be distributed across shards
        // deterministically.
        const idStart = i * 4;
        const docs = [
            {_id: idStart, k: i, n: 1e+34},
            {_id: idStart + 1, k: i, n: NumberDecimal("0.1")},
            {_id: idStart + 2, k: i, n: NumberDecimal("0.01")},
            {_id: idStart + 3, k: i, n: -1e+34}
        ];
        assert.commandWorked(hashShardedColl.insert(docs));
    }

    const pipelineWithSum = [{$group: {_id: "$k", s: {$sum: "$n"}}}, {$group: {_id: "$s"}}];
    const pipelineWithAvg = [{$group: {_id: "$k", s: {$avg: "$n"}}}, {$group: {_id: "$s"}}];

    const expectedResSum = [{"_id": NumberDecimal("0.11")}];
    verifyShardedAccumulatorResultsOnBothEngine(
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.neq(
            actualRes,
            expectedResSum,
            `Sharded sum for mixed data by which only decimal sum survive on ${version}: \n` +
                `${tojson(actualRes)} == ${tojson(expectedResSum)}`));

    const expectedResAvg = [{"_id": NumberDecimal("0.0275")}];
    verifyShardedAccumulatorResultsOnBothEngine(
        hashShardedColl,
        pipelineWithAvg,
        (actualRes) => assert.neq(
            actualRes,
            expectedResAvg,
            `Sharded avg for mixed data by which only decimal sum survive on ${version}: \n` +
                `${tojson(actualRes)} == ${tojson(expectedResAvg)}`));

    // Upgrade the cluster to the latest.
    st.upgradeCluster(
        "latest",
        {upgradeShards: true, upgradeConfigs: true, upgradeMongos: true, waitUntilStable: true});

    db = st.getDB(jsTestName());
    checkFCV(st.rs0.getPrimary().getDB("admin"), lastContinuousFCV);

    hashShardedColl = db.partial_sum;

    // $sum fix is FCV-gated. So, it's not applied after binary upgrade.
    verifyShardedAccumulatorResultsOnBothEngine(
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.neq(
            actualRes,
            expectedResSum,
            "Sharded sum for mixed data by which only decimal sum survive on latest after binary upgrade: \n" +
                `${tojson(actualRes)} == ${tojson(expectedResSum)}`));

    // On the other hand, $avg fix is not FCV-gated. So, it's applied after binary upgrade.
    verifyShardedAccumulatorResultsOnBothEngine(
        hashShardedColl,
        pipelineWithAvg,
        (actualRes) => assert.eq(
            actualRes,
            expectedResAvg,
            "Sharded avg for mixed data by which only decimal sum survive on latest after binary upgrade: \n" +
                `${tojson(actualRes)} != ${tojson(expectedResAvg)}`));

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    // The FCV is upgraded to the 'latestFCV' and $sum fix must be applied now.
    verifyShardedAccumulatorResultsOnBothEngine(
        hashShardedColl,
        pipelineWithSum,
        (actualRes) => assert.eq(
            actualRes,
            expectedResSum,
            "Sharded sum for mixed data by which only decimal sum survive on latest after FCV upgrade: \n" +
                `${tojson(actualRes)} != ${tojson(expectedResSum)}`));

    st.stop();
}());
}());
