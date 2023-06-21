/**
 * Tests that the analyzeShardKey command correctly handles the case where the fast data statistics
 * from $collStats indicate that the collection is empty when it is not (after an unclean shutdown).
 *
 * This test is not compatible with the config shard suites because it involves killing the primary
 * node of the only shard in the cluster; in the config shard suites, the shard would be the config
 * shard, therefore the node would need to be restarted as a configsvr and then transitioned to be
 * a config shard node. The ShardingTest and ReplSetTest API currently doesn't support doing that.
 *
 * @tags: [requires_fcv_70, requires_persistence, config_shard_incompatible]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const numMostCommonValues = 5;
const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues
};

function runTest(conn, {rst, st}) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;

    let db = conn.getDB(dbName);
    let coll = db.getCollection(collName);

    // Make each document in the collection have two fields "x" and "y" where "x" is not unique and
    // "y" is. Verify the metrics for the shard key {x: 1} and {y: 1} before and after an unclean
    // shutdown which results in inaccurate fast data statistics.
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [
            {
                name: "x_1",
                key: {x: 1},
            },
            {
                name: "y_1",
                key: {y: 1},
                unique: true,
            }
        ]
    }));

    const docs = [];
    const mostCommonXValues = [];
    const mostCommonYValues = [];

    const numDistinctXValues = 10;
    for (let i = 1; i <= numDistinctXValues; i++) {
        const xValue = i;
        const xFrequency = i;
        for (let j = 0; j < xFrequency; j++) {
            const yValue = UUID();
            const doc = {_id: UUID(), x: xValue, y: yValue};
            docs.push(doc);
            mostCommonYValues.push({value: {y: yValue}, frequency: 1});
        }
        const isMostCommonXValue = (numDistinctXValues - i) < numMostCommonValues;
        if (isMostCommonXValue) {
            mostCommonXValues.push({value: {x: xValue}, frequency: xFrequency});
        }
    }
    const numDistinctYValues = docs.length;
    assert.commandWorked(coll.insert(docs));

    jsTest.log("Verify that the analyzeShardKey metrics prior to the unclean shutdown");

    const resXBefore = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {x: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resXBefore.keyCharacteristics, {
        numDocs: docs.length,
        isUnique: false,
        numDistinctValues: numDistinctXValues,
        mostCommonValues: mostCommonXValues,
        numMostCommonValues
    });
    assert.eq(resXBefore.keyCharacteristics.avgDocSizeBytes, Object.bsonsize(docs[0]));

    const resYBefore = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {y: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resYBefore.keyCharacteristics, {
        numDocs: docs.length,
        isUnique: true,
        numDistinctValues: numDistinctYValues,
        mostCommonValues: mostCommonYValues,
        numMostCommonValues
    });
    assert.eq(resYBefore.keyCharacteristics.avgDocSizeBytes, Object.bsonsize(docs[0]));

    assert(rst || st);
    const rstToKill = rst ? rst : st.rs0;
    const nodeToKill = rstToKill.getPrimary();

    jsTest.log("Waiting for the writes to get checkpointed on the mongod that needs to be killed");
    sleep(2000);

    jsTest.log("Killing and restart the mongod");
    rstToKill.stop(nodeToKill, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
    const restartOptions = {noCleanData: true, setParameter: setParameterOpts};
    if (st) {
        restartOptions.shardsvr = "";
    }
    rstToKill.start(nodeToKill, restartOptions);

    if (rst) {
        conn = rstToKill.getPrimary();
        db = conn.getDB(dbName);
        coll = db.getCollection(collName);
    } else {
        rstToKill.getPrimary();
    }

    jsTest.log("Verify that the fast data statistics are inaccurate after the unclean shutdown");
    const collStatsAfter =
        coll.aggregate([
                {$collStats: {storageStats: {}}},
                {$project: {count: "$storageStats.count", size: "$storageStats.size"}}
            ])
            .toArray()[0];
    assert.eq(collStatsAfter.count, 0);
    assert.eq(collStatsAfter.size, 0);

    jsTest.log("Verify the analyzeShardKey metrics after the unclean shutdown");

    // The cardinality and frequency metrics for {x: 1} should be accurate since the metrics
    // calculation for a shard key that is not unique does not depend on fast count. However, the
    // average document size should be set to the size of an empty document since that information
    // is not available from $collStats.
    const resXAfter = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {x: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resXAfter.keyCharacteristics, {
        numDocs: docs.length,
        isUnique: false,
        numDistinctValues: numDistinctXValues,
        mostCommonValues: mostCommonXValues,
        numMostCommonValues
    });
    assert.eq(resXAfter.keyCharacteristics.avgDocSizeBytes, Object.bsonsize({}));

    // The cardinality and frequency metrics for {y: 1} should be inaccurate since the metrics
    // calculation for a shard key that is unique depends on fast count (this is optimization that
    // unfortunately does not work out correctly in this rare case). However, the metrics should
    // still make sense. That is, the number of documents and the number distinct values should not
    // be zero and instead should be equal to the number of most common values returned. Similar to
    // previous case, the average document size should be set to the size of an empty document since
    // that information is not available from $collStats.
    const resYAfter = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {y: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resYAfter.keyCharacteristics, {
        numDocs: numMostCommonValues,
        isUnique: true,
        numDistinctValues: numMostCommonValues,
        mostCommonValues: mostCommonYValues,
        numMostCommonValues
    });
    assert.eq(resYAfter.keyCharacteristics.avgDocSizeBytes, Object.bsonsize({}));

    let runAnalyzeShardKeyCmd = (host, ns, key) => {
        const conn = new Mongo(host);
        return conn.adminCommand({
            analyzeShardKey: ns,
            key: key,
            // Skip calculating the read and write distribution metrics since there are not needed
            // by this test.
            readWriteDistribution: false
        });
    };

    for (let shardKey of [{x: 1}, {y: 1}]) {
        jsTest.log(
            "Verify that the analyzeShardKey command fails when the collection is actually empty " +
            tojson({shardKey}));
        const analyzeShardKeyThread = new Thread(runAnalyzeShardKeyCmd, conn.host, ns, shardKey);
        const fp =
            configureFailPoint(st ? st.rs0.nodes[0] : rst.nodes[0],
                               "analyzeShardKeyPauseBeforeCalculatingKeyCharacteristicsMetrics");
        analyzeShardKeyThread.start();
        fp.wait();
        // Delete all documents in the collection.
        assert.commandWorked(coll.remove({}));
        fp.off();
        assert.commandFailedWithCode(analyzeShardKeyThread.returnData(),
                                     ErrorCodes.IllegalOperation);
        // Reinsert the documents.
        assert.commandWorked(coll.insert(docs));
    }
}

{
    const st =
        new ShardingTest({shards: 1, rs: {nodes: 1, setParameter: setParameterOpts, syncdelay: 1}});
    runTest(st.s, {st});
    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: 1, nodeOptions: {setParameter: setParameterOpts, syncdelay: 1}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    runTest(primary, {rst});
    rst.stopSet();
}
})();
