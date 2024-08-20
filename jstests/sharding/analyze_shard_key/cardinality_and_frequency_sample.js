/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics when
 * document sampling is involved.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {
    assertAggregateQueryPlans,
    getMongodConns,
    numMostCommonValues
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common.js";

const numNodesPerRS = 2;

const batchSize = 1000;
// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

const defaultSampleSize = 10000;
const numDocsTotal = 50000;

const sampleSizeTestCases = [
    {sampleSize: Math.floor(0.8 * numDocsTotal)},
    {sampleSize: numDocsTotal},
    {sampleSize: Math.floor(1.5 * numDocsTotal)},
    {
        sampleSize: Math.floor(0.5 * numMostCommonValues),
        expectedErrCodes: [
            // The number of sampled documents for the monotonicity step is 0 because the sample
            // rate is too low.
            7826505,
            // The number of sampled documents for the monotonicity step is greater than 0 so
            // the command fails the validation in the cardinality and frequency step because
            // the requested sample size is less than 'numMostCommonValues'.
            ErrorCodes.InvalidOptions,
        ]
    }
];

function makeSampleRateTestCases(isUnique) {
    return [
        {sampleRate: 0.35},
        {sampleRate: 1},
        {
            // The expected sample size is 1, which is less than 'numMostCommonValues' (defaults to
            // 5).
            sampleRate: 1 / numDocsTotal,
            expectedErrCodes: [
                // The number of sampled documents for the monotonicity step is 0 because the sample
                // rate is too low.
                7826505,
                // The number of sampled documents for the monotonicity step is greater than 0 but
                // the number of sampled documents for the cardinality and frequency step is 0.
                isUnique ? 7826506 : 7826507,
            ]
        },
        {
            // The expected sample size is less than 1, which is less than 'numMostCommonValues'.
            sampleRate: 0.1 / numDocsTotal,
            expectedErrCodes: [
                // The number of sampled documents for the monotonicity step is 0 because the sample
                // rate is too low.
                7826505,
                // The number of sampled documents for the monotonicity step is greater than 0 but
                // the number of sampled documents for the cardinality and frequency step is 0.
                isUnique ? 7826506 : 7826507,
            ]
        },
    ];
}
const sampleSizeTestCasesUnique = makeSampleRateTestCases(true /* isUnique */);
const sampleSizeTestCasesNotUnique = makeSampleRateTestCases(false /* isUnique */);

function runTest(conn, {isUnique, isShardedColl, st, rst}) {
    const dbName = "testDb";
    const collName = "testColl";
    if (st) {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
    }
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);
    const mongodConns = getMongodConns({st, rst});

    jsTest.log("Testing the test cases for " + tojsononeline({isUnique, isShardedColl}));

    const indexOptions = isUnique ? {unique: true} : {};
    assert.commandWorked(coll.createIndex({a: 1}, indexOptions));
    const isClusteredColl = AnalyzeShardKeyUtil.isClusterCollection(conn, dbName, collName);

    if (isShardedColl) {
        assert(st);
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {a: "hashed"}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {a: 1}, to: st.shard1.shardName}));
    }

    // Insert documents for this collection.
    let docsBuffer = [];
    let numDocsInserted = 0;
    const bufferOrInsertDoc = (doc) => {
        docsBuffer.push(doc);
        if (docsBuffer.length == batchSize) {
            assert.commandWorked(coll.insert(docsBuffer, {writeConcern}));
            numDocsInserted += docsBuffer.length;
            docsBuffer = [];
        }
    };

    // Only set if the shard key is unique.
    let mostCommonValue0, mostCommonRatio0, mostCommonValue1, mostCommonRatio1;
    if (!isUnique) {
        // The documents for the most common value.
        mostCommonValue0 = -87654321;
        mostCommonRatio0 = 0.5;
        for (let i = 0; i < (numDocsTotal * mostCommonRatio0); i++) {
            bufferOrInsertDoc({a: mostCommonValue0});
        }
        // The documents for the second most common value.
        mostCommonValue1 = -12345678;
        mostCommonRatio1 = 0.25;
        for (let i = 0; i < (numDocsTotal * mostCommonRatio1); i++) {
            bufferOrInsertDoc({a: mostCommonValue1});
        }
    }
    // The other documents.
    let aValue = 1;
    while ((numDocsInserted + docsBuffer.length) < numDocsTotal) {
        bufferOrInsertDoc({a: aValue++});
    }

    // Insert any remaining docs in the buffer.
    assert.commandWorked(coll.insert(docsBuffer, {writeConcern}));
    assert.eq(coll.find().itcount(), numDocsTotal);

    const ratioMaxDiff = 0.1;
    const checkMostCommonValuesFn = (metrics) => {
        let startIndex = 0;
        if (!isUnique) {
            const mostCommon0 = metrics.mostCommonValues[0];
            assert.eq(mostCommon0.value, {a: mostCommonValue0}, metrics);
            AnalyzeShardKeyUtil.assertApprox(mostCommon0.frequency / metrics.numDocsSampled,
                                             mostCommonRatio0,
                                             {metrics},
                                             ratioMaxDiff);
            const mostCommon1 = metrics.mostCommonValues[1];
            assert.eq(mostCommon1.value, {a: mostCommonValue1}, metrics);
            AnalyzeShardKeyUtil.assertApprox(mostCommon1.frequency / metrics.numDocsSampled,
                                             mostCommonRatio1,
                                             {metrics},
                                             ratioMaxDiff);

            startIndex += 2;
        }
        for (let i = startIndex; i < metrics.mostCommonValues.length; i++) {
            const mostCommon = metrics.mostCommonValues[i];
            assert.lte(Math.abs(mostCommon.value.a), aValue, metrics);
            assert.eq(mostCommon.frequency, 1, metrics);
        }
    };

    AnalyzeShardKeyUtil.enableProfiler(mongodConns, dbName);

    for (let isHashed of [false, true]) {
        if (isHashed && isUnique) {
            // Hashed indexes cannot have a uniqueness constraint.
            continue;
        }

        const shardKey = {a: isHashed ? "hashed" : 1};
        const monotonicityType =
            isClusteredColl ? "unknown" : (isHashed ? "not monotonic" : "monotonic");

        const comment = UUID();

        // Cannot specify both sampleRate and sampleSize.
        assert.commandFailedWithCode(conn.adminCommand({
            analyzeShardKey: ns,
            key: shardKey,
            comment,
            sampleRate: 0.5,
            sampleSize: 10000,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false
        }),
                                     ErrorCodes.InvalidOptions);

        // sampleSize < numTotalDocs (default).
        jsTest.log("Testing default 'sampleSize': " +
                   tojsononeline({defaultSampleSize, isHashed, isUnique, isShardedColl}));

        const res = assert.commandWorked(conn.adminCommand({
            analyzeShardKey: ns,
            key: shardKey,
            comment: comment,
            // Skip calculating the read and write distribution metrics since they are not needed by
            // this test.
            readWriteDistribution: false
        }));
        jsTest.log("Response for default 'sampleSize': " + tojsononeline({defaultSampleSize, res}));
        const metrics = res.keyCharacteristics;

        AnalyzeShardKeyUtil.validateKeyCharacteristicsMetrics(metrics);
        assert.eq(metrics.numDocsTotal, numDocsTotal, res);
        assert.lte(metrics.numDocsSampled, defaultSampleSize, metrics);
        checkMostCommonValuesFn(metrics);
        assert.eq(metrics.monotonicity.type, monotonicityType, metrics);
        assertAggregateQueryPlans(mongodConns,
                                  dbName,
                                  collName,
                                  comment,
                                  // On a replica set, the analyzeShardKey command runs the
                                  // aggregate commands locally, i.e. the commands do not go
                                  // through the service entry point so do not get profiled.
                                  !rst /* expectEntries */);

        for (let {sampleSize, expectedErrCodes} of sampleSizeTestCases) {
            jsTest.log("Testing custom 'sampleSize': " +
                       tojsononeline({sampleSize, isHashed, isUnique, isShardedColl}));
            const comment = UUID();
            const res = conn.adminCommand({
                analyzeShardKey: ns,
                key: shardKey,
                sampleSize,
                comment: comment,
                // Skip calculating the read and write distribution metrics since they are not
                // needed by this test.
                readWriteDistribution: false
            });
            jsTest.log("Response custom 'sampleSize': " + tojsononeline({sampleSize, res}));

            if (expectedErrCodes) {
                assert.commandFailedWithCode(res, expectedErrCodes);
                continue;
            }
            assert.commandWorked(res);
            const metrics = res.keyCharacteristics;

            AnalyzeShardKeyUtil.validateKeyCharacteristicsMetrics(metrics);
            assert.eq(metrics.numDocsTotal, numDocsTotal, res);
            assert.lte(res.keyCharacteristics.numDocsSampled, sampleSize, res);
            if (expectedErrCodes) {
                continue;
            }
            checkMostCommonValuesFn(metrics);
            assert.eq(metrics.monotonicity.type, monotonicityType, metrics);
            assertAggregateQueryPlans(mongodConns,
                                      dbName,
                                      collName,
                                      comment,
                                      // On a replica set, the analyzeShardKey command runs the
                                      // aggregate commands locally, i.e. the commands do not go
                                      // through the service entry point so do not get profiled.
                                      !rst /* expectEntries */);
        }

        const sampleRateTestCases =
            isUnique ? sampleSizeTestCasesUnique : sampleSizeTestCasesNotUnique;
        for (let {sampleRate, expectedErrCodes} of sampleRateTestCases) {
            jsTest.log("Testing custom 'sampleRate': " +
                       tojsononeline({sampleRate, isHashed, isUnique, isShardedColl}));
            const comment = UUID();
            const res = conn.adminCommand({
                analyzeShardKey: ns,
                key: shardKey,
                sampleRate,
                comment: comment,
                // Skip calculating the read and write distribution metrics since they are not
                // needed by this test.
                readWriteDistribution: false
            });
            jsTest.log("Response for custom 'sampleRate': " + tojsononeline({sampleRate, res}));

            const sampleSize = Math.ceil(sampleRate * numDocsTotal);
            if (!res.ok) {
                assert.lte(sampleSize, 1);
                assert.commandFailedWithCode(res, expectedErrCodes);
                continue;
            }
            assert.commandWorked(res);
            const metrics = res.keyCharacteristics;

            AnalyzeShardKeyUtil.validateKeyCharacteristicsMetrics(metrics);
            assert.eq(metrics.numDocsTotal, numDocsTotal, res);
            assert.lte(metrics.numDocsSampled, sampleSize, metrics);
            if (expectedErrCodes) {
                continue;
            }
            checkMostCommonValuesFn(metrics);
            assert.eq(metrics.monotonicity.type, monotonicityType, metrics);
            assertAggregateQueryPlans(mongodConns,
                                      dbName,
                                      collName,
                                      comment,
                                      // On a replica set, the analyzeShardKey command runs the
                                      // aggregate commands locally, i.e. the commands do not go
                                      // through the service entry point so do not get profiled.
                                      !rst /* expectEntries */);
        }
    }

    AnalyzeShardKeyUtil.disableProfiler(mongodConns, dbName);
    assert(coll.drop());
}

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
    analyzeShardKeyCharacteristicsDefaultSampleSize: defaultSampleSize
};

{
    const st =
        new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    for (let isShardedColl of [false, true]) {
        runTest(st.s, {isUnique: true, isShardedColl, st});
        runTest(st.s, {isUnique: false, isShardedColl, st});
    }

    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary, {isUnique: true, isShardedColl: false, rst});
    runTest(primary, {isUnique: false, isShardedColl: false, rst});

    rst.stopSet();
}