/**
 * Tests that the analyzeShardKey command correctly return the value and frequency of the most
 * common shard key values when their size exceeds the BSONObj size limit.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

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

function runTest(conn, {isUnique, isShardedColl, st}) {
    const dbName = "testDb";
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
    assert.commandWorked(coll.createIndex({a: 1}, indexOptions));
    assert.commandWorked(coll.createIndex({"a.y": 1}, indexOptions));
    assert.commandWorked(coll.createIndex({"a.y.ii": 1}, indexOptions));

    if (isShardedColl) {
        assert(!isUnique,
               "Cannot test with a sharded collection when the candidate shard keys " +
                   "are unique since uniqueness can't be maintained unless the shard key is " +
                   "prefix of the candidate shard keys");
        assert(st);
        assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
        st.ensurePrimaryShard(dbName, st.shard0.name);

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

    // Verify the analyzeShardKey command truncates large primitive type fields.
    const res0 = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: {"a.y.ii": 1}}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res0, {
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
    });

    // Verify the analyzeShardKey command truncates large primitive type subfields.
    const res1 = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: {"a.y": 1}}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res1, {
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
    });

    // Verify the analyzeShardKey command truncates large object type subfields.
    const res2 = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: {a: 1}}));
    AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res2, {
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
    });

    assert(coll.drop());
}

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
    // Skip calculating the read and write distribution metrics since there are no sampled queries
    // anyway.
    "failpoint.analyzeShardKeySkipCalcalutingReadWriteDistributionMetrics":
        tojson({mode: "alwaysOn"})
};

{
    const st =
        new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    runTest(st.s, {isUnique: true, isShardedColl: false});
    runTest(st.s, {isUnique: false, isShardedColl: false});
    runTest(st.s, {isUnique: false, isShardedColl: true, st});

    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary, {isUnique: true, isShardedColl: false});
    runTest(primary, {isUnique: false, isShardedColl: false});

    rst.stopSet();
}
})();
