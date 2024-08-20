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
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

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

    const expectedMetricsX = {
        numDocs: docs.length,
        isUnique: false,
        numDistinctValues: numDistinctXValues,
        mostCommonValues: mostCommonXValues,
        numMostCommonValues
    };
    const expectedMetricsY = {
        numDocs: docs.length,
        isUnique: true,
        numDistinctValues: numDistinctYValues,
        mostCommonValues: mostCommonYValues,
        numMostCommonValues
    };
    const expectedAvgDocSize = Object.bsonsize(docs[0]);

    const resXBefore = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {x: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resXBefore.keyCharacteristics,
                                                        expectedMetricsX);
    assert.eq(resXBefore.keyCharacteristics.avgDocSizeBytes, expectedAvgDocSize);

    const resYBefore = assert.commandWorked(conn.adminCommand({
        analyzeShardKey: ns,
        key: {y: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    }));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resYBefore.keyCharacteristics,
                                                        expectedMetricsY);
    assert.eq(resYBefore.keyCharacteristics.avgDocSizeBytes, expectedAvgDocSize);

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
            "Verify that the analyzeShardKey command fails when the collection becomes empty " +
            "right before the $collStats step " + tojson({shardKey}));
        const analyzeShardKeyThread = new Thread(runAnalyzeShardKeyCmd, conn.host, ns, shardKey);
        const fp = configureFailPoint(st ? st.rs0.nodes[0] : rst.nodes[0],
                                      "analyzeShardKeyPauseBeforeCalculatingCollStatsMetrics");
        analyzeShardKeyThread.start();
        fp.wait();
        // Delete all documents in the collection.
        assert.commandWorked(coll.remove({}));
        fp.off();
        assert.commandFailedWithCode(analyzeShardKeyThread.returnData(), 7826501);
        // Reinsert the documents.
        assert.commandWorked(coll.insert(docs));
    }

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

    jsTest.log("Verify that the analyzeShardKey command fails if the fast data statistics " +
               "checked in the $collStats step or monotonicity step indicate that the " +
               "collection is empty (although it is not) after the unclean shutdown");
    const collStatsAfter =
        coll.aggregate([
                {$collStats: {storageStats: {}}},
                {$project: {count: "$storageStats.count", size: "$storageStats.size"}}
            ])
            .toArray()[0];

    jsTest.log("Verify the analyzeShardKey metrics after the unclean shutdown");

    const resXAfter = conn.adminCommand({
        analyzeShardKey: ns,
        key: {x: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    });
    const resYAfter = conn.adminCommand({
        analyzeShardKey: ns,
        key: {y: 1},
        // Skip calculating the read and write distribution metrics since there are not needed by
        // this test.
        readWriteDistribution: false
    });

    if (collStatsAfter.count == 0) {
        assert.eq(collStatsAfter.size, 0);
        // IllegalOperation is the error thrown by the monotonicity step, whereas 7826501 is the
        // error thrown in the $collStats step. Currently, the monotonicity step comes before the
        // $collStats step and there is no monotonicity check for clustered collections.
        const expectedErrCode = AnalyzeShardKeyUtil.isClusterCollection(conn, dbName, collName)
            ? 7826501
            : ErrorCodes.IllegalOperation;
        assert.commandFailedWithCode(resXAfter, expectedErrCode);
        assert.commandFailedWithCode(resYAfter, expectedErrCode);
    } else {
        assert.gt(collStatsAfter.size, 0);
        assert.commandWorked(resXAfter);
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resXAfter.keyCharacteristics,
                                                            expectedMetricsX);
        assert.eq(resXAfter.keyCharacteristics.avgDocSizeBytes, expectedAvgDocSize);

        assert.commandWorked(resYAfter);
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(resYAfter.keyCharacteristics,
                                                            expectedMetricsY);
        assert.eq(resYAfter.keyCharacteristics.avgDocSizeBytes, expectedAvgDocSize);
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
