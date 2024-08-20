/**
 * Tests that the analyzeShardKey command correctly return the value and frequency of the most
 * common shard key values when their size exceeds the BSONObj size limit.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    AnalyzeShardKeyUtil
} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";

const kSize10MB = 10 * 1024 * 1024;

const numNodesPerRS = 2;
const numMostCommonValues = 5;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

const simpleCollation = {
    locale: "simple"
};
const caseInsensitiveCollation = {
    locale: "en_US",
    strength: 1,
    caseLevel: false
};

function setMongodServerParametersReplicaSet(rst, params) {
    rst.nodes.forEach(node => {
        assert.commandWorked(node.adminCommand(Object.assign({setParameter: 1}, params)));
    });
}

function setMongodServerParametersShardedCluster(st, params) {
    st._rs.forEach(rst => {
        setMongodServerParametersReplicaSet(rst, params);
    });
}

function setMongodServerParameters({st, rst, params}) {
    if (st) {
        setMongodServerParametersShardedCluster(st, params);
    } else if (rst) {
        setMongodServerParametersReplicaSet(rst, params);
    }
}

function runTest(conn, {isHashed, isUnique, isShardedColl, st, rst}) {
    assert(!isHashed || !isUnique);
    jsTest.log("Testing the test cases for " + tojson({isHashed, isUnique, isShardedColl}));

    const dbName = "testDb";
    if (st) {
        assert.commandWorked(
            st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    }
    const collName = "testColl";
    const ns = dbName + "." + collName;
    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    // To verify that the metrics calculation uses simple collation, make the collection have
    // case-sensitive default collation and make some of its documents have fields that only differ
    // in casing.
    assert.commandWorked(db.createCollection(collName, {collation: caseInsensitiveCollation}));

    const indexOptions =
        Object.assign({collation: simpleCollation}, isUnique ? {unique: true} : {});
    assert.commandWorked(coll.createIndex({a: isHashed ? "hashed" : 1}, indexOptions));
    assert.commandWorked(coll.createIndex({"a.y": isHashed ? "hashed" : 1}, indexOptions));
    assert.commandWorked(coll.createIndex({"a.y.ii": isHashed ? "hashed" : 1}, indexOptions));

    if (isShardedColl) {
        assert(!isUnique,
               "Cannot test with a sharded collection when the candidate shard keys " +
                   "are unique since uniqueness can't be maintained unless the shard key is " +
                   "prefix of the candidate shard keys");
        assert(st);

        // Make the collection have two chunks:
        // shard0: [MinKey, 1]
        // shard1: [1, MaxKey]
        assert.commandWorked(
            st.s.adminCommand({shardCollection: ns, key: {b: 1}, collation: simpleCollation}));
        assert.commandWorked(st.s.adminCommand({split: ns, middle: {b: 1}}));
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {b: 1}, to: st.shard1.shardName}));
    }

    assert.commandWorked(
        coll.insert({a: {x: -2, y: {i: -2, ii: "a", iii: -2}, z: -2}, b: -2}, {writeConcern}));
    assert.commandWorked(
        coll.insert({a: {x: -1, y: {i: -1, ii: "A", iii: -1}, z: -1}, b: -1}, {writeConcern}));
    assert.commandWorked(
        coll.insert({a: {x: 0, y: {i: 0, ii: new Array(kSize10MB).join("B"), iii: 0}, z: 0}, b: 0},
                    {writeConcern}));
    assert.commandWorked(
        coll.insert({a: {x: 1, y: {i: 1, ii: new Array(kSize10MB).join("C"), iii: 1}, z: 1}, b: 1},
                    {writeConcern}));
    assert.commandWorked(
        coll.insert({a: {x: 2, y: {i: 2, ii: new Array(kSize10MB).join("D"), iii: 2}, z: 2}, b: 2},
                    {writeConcern}));

    const testCases = [];

    // Verify the analyzeShardKey command truncates large primitive type fields.
    const cmdObj0 = {
        analyzeShardKey: ns,
        key: {"a.y.ii": 1},
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    };
    const expectedMetrics0 = {
        numDocs: 5,
        isUnique,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {"a.y.ii": "a"}, frequency: 1},
            {value: {"a.y.ii": "A"}, frequency: 1},
            {
                value: {"a.y.ii": {type: "string", value: "truncated", sizeBytes: 10485764}},
                frequency: 1
            },
            {
                value: {"a.y.ii": {type: "string", value: "truncated", sizeBytes: 10485764}},
                frequency: 1
            },
            {
                value: {"a.y.ii": {type: "string", value: "truncated", sizeBytes: 10485764}},
                frequency: 1
            },
        ],
        numMostCommonValues
    };
    testCases.push({cmdObj: cmdObj0, expectedMetrics: expectedMetrics0});

    // Verify the analyzeShardKey command truncates large primitive type subfields.
    const cmdObj1 = {
        analyzeShardKey: ns,
        key: {"a.y": 1},
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    };
    const expectedMetrics1 = {
        numDocs: 5,
        isUnique,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {"a.y": {i: -2, ii: "a", iii: -2}}, frequency: 1},
            {value: {"a.y": {i: -1, ii: "A", iii: -1}}, frequency: 1},
            {
                value: {
                    "a.y": {
                        i: 0,
                        ii: {type: "string", value: "truncated", sizeBytes: 10485764},
                        iii: 0
                    }
                },
                frequency: 1
            },
            {
                value: {
                    "a.y": {
                        i: 1,
                        ii: {type: "string", value: "truncated", sizeBytes: 10485764},
                        iii: 1
                    }
                },
                frequency: 1
            },
            {
                value: {
                    "a.y": {
                        i: 2,
                        ii: {type: "string", value: "truncated", sizeBytes: 10485764},
                        iii: 2
                    }
                },
                frequency: 1
            },
        ],
        numMostCommonValues
    };
    testCases.push({cmdObj: cmdObj1, expectedMetrics: expectedMetrics1});

    // Verify the analyzeShardKey command truncates large object type subfields.
    const cmdObj2 = {
        analyzeShardKey: ns,
        key: {a: 1},
        // Skip calculating the read and write distribution metrics since they are not needed by
        // this test.
        readWriteDistribution: false
    };
    const expectedMetrics2 = {
        numDocs: 5,
        isUnique,
        numDistinctValues: 5,
        mostCommonValues: [
            {value: {a: {x: -2, y: {i: -2, ii: "a", iii: -2}, z: -2}}, frequency: 1},
            {value: {a: {x: -1, y: {i: -1, ii: "A", iii: -1}, z: -1}}, frequency: 1},
            {
                value:
                    {a: {x: 0, y: {type: "object", value: "truncated", sizeBytes: 10485797}, z: 0}},
                frequency: 1
            },
            {
                value:
                    {a: {x: 1, y: {type: "object", value: "truncated", sizeBytes: 10485797}, z: 1}},
                frequency: 1
            },
            {
                value:
                    {a: {x: 2, y: {type: "object", value: "truncated", sizeBytes: 10485797}, z: 2}},
                frequency: 1
            },
        ],
        numMostCommonValues
    };
    testCases.push({cmdObj: cmdObj2, expectedMetrics: expectedMetrics2});

    const sufficientAccumulatorBytesLimitParams = {
        internalQueryTopNAccumulatorBytes: kSize10MB * 15,
    };
    const insufficientAccumulatorBytesLimitParams = {
        internalQueryTopNAccumulatorBytes: kSize10MB,
    };

    for (let {cmdObj, expectedMetrics} of testCases) {
        jsTest.log("Testing " + tojson({isHashed, isUnique, isShardedColl, cmdObj}));

        setMongodServerParameters({st, rst, params: sufficientAccumulatorBytesLimitParams});
        let res = conn.adminCommand(cmdObj);
        assert.commandWorked(res);
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics,
                                                            expectedMetrics);

        setMongodServerParameters({st, rst, params: insufficientAccumulatorBytesLimitParams});
        res = conn.adminCommand(cmdObj);
        if (isUnique || isHashed) {
            assert.commandWorked(res);
            AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res.keyCharacteristics,
                                                                expectedMetrics);
        } else {
            // The aggregation pipeline that the analyzeShardKey command uses to calculate the
            // cardinality and frequency metrics when the supporting index is not unique contains
            // a $group stage with $topN. The small size limit for $topN would therefore cause
            // the analyzeShardKey command to fail with an ExceededMemoryLimit error when the
            // index (i.e. values to group and sort) is not hashed.
            assert.commandFailedWithCode(res, ErrorCodes.ExceededMemoryLimit);
        }
    }

    assert(coll.drop());
}

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
};

{
    const st =
        new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    runTest(st.s, {isHashed: false, isUnique: true, isShardedColl: false, st});
    runTest(st.s, {isHashed: false, isUnique: false, isShardedColl: false, st});
    // Not testing unique hashed index since hashed indexes cannot have a uniqueness constraint.
    runTest(st.s, {isHashed: true, isUnique: false, isShardedColl: false, st});

    // Not testing unique b-tree index since uniqueness can't be maintained unless the shard key
    // is prefix of the candidate shard keys.
    runTest(st.s, {isHashed: false, isUnique: false, isShardedColl: true, st});
    // Not testing unique hashed index since hashed indexes cannot have a uniqueness constraint.
    runTest(st.s, {isHashed: true, isUnique: false, isShardedColl: true, st});

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary, {isHashed: false, isUnique: true, isShardedColl: false, rst});
    runTest(primary, {isHashed: false, isUnique: false, isShardedColl: false, rst});
    // Not testing unique hashed index since hashed indexes cannot have a uniqueness constraint.
    runTest(primary, {isHashed: true, isUnique: false, isShardedColl: false, rst});

    rst.stopSet();
}