/**
 * Tests that the cardinality and frequency metrics calculation within the analyzeShardKey command
 * prioritizes indexes that allows it to infer if the shard key is unique.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/uuid_util.js");  // for 'extractUUIDFromObject'
load("jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js");

const numNodesPerRS = 2;
const numMostCommonValues = 5;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

function testAnalyzeShardKey(conn, {docs, indexSpecs, shardKeys, metrics}) {
    const dbName = "testDb-" + extractUUIDFromObject(UUID());
    const collName = "testColl";
    const ns = dbName + "." + collName;
    jsTest.log(`Testing ${tojson({dbName, collName, docs, indexSpecs, shardKeys, metrics})}`);

    const db = conn.getDB(dbName);
    const coll = db.getCollection(collName);

    assert.commandWorked(db.runCommand({createIndexes: collName, indexes: indexSpecs}));
    assert.commandWorked(coll.insert(docs, {writeConcern}));

    for (let shardKey of shardKeys) {
        const res = assert.commandWorked(conn.adminCommand({analyzeShardKey: ns, key: shardKey}));
        AnalyzeShardKeyUtil.assertKeyCharacteristicsMetrics(res, metrics);
    }
}

function runTest(conn) {
    testAnalyzeShardKey(conn, {
        docs: [{x: -1}, {x: 1}],
        indexSpecs: [
            {
                name: "x_hashed",
                key: {x: "hashed"},
            },
            {
                name: "x_1",
                key: {x: 1},
                unique: true,
            }
        ],
        shardKeys: [{x: 1}, {x: "hashed"}],
        metrics: {
            numDocs: 2,
            isUnique: true,
            numDistinctValues: 2,
            mostCommonValues: [{value: {x: -1}, frequency: 1}, {value: {x: 1}, frequency: 1}],
            numMostCommonValues
        }
    });

    testAnalyzeShardKey(conn, {
        docs: [{x: -1}, {x: 1}],
        indexSpecs: [
            {
                name: "x_1_not_unique",
                key: {x: 1},
            },
            {
                name: "x_1_unique",
                key: {x: 1},
                unique: true,
            }
        ],
        shardKeys: [{x: 1}, {x: "hashed"}],
        metrics: {
            numDocs: 2,
            isUnique: true,
            numDistinctValues: 2,
            mostCommonValues: [{value: {x: -1}, frequency: 1}, {value: {x: 1}, frequency: 1}],
            numMostCommonValues
        }
    });

    testAnalyzeShardKey(conn, {
        docs: [{x: -1, y: -1}, {x: 1, y: 1}],
        indexSpecs: [
            {
                name: "x_1_y_1",
                key: {x: 1, y: 1},
                unique: true,
            },
            {
                name: "x_1",
                key: {x: 1},
            }
        ],
        shardKeys: [{x: 1}, {x: "hashed"}],
        metrics: {
            numDocs: 2,
            isUnique: false,
            numDistinctValues: 2,
            mostCommonValues: [{value: {x: -1}, frequency: 1}, {value: {x: 1}, frequency: 1}],
            numMostCommonValues
        }
    });

    testAnalyzeShardKey(conn, {
        docs: [{x: -1, y: -1, z: -1}, {x: 1, y: 1, z: 1}],
        indexSpecs: [
            {
                name: "x_1_y_1_z_1",
                key: {x: 1, y: 1, z: 1},
                unique: true,
            },
            {
                name: "x_1_y_1",
                key: {x: 1, y: 1},
                unique: true,
            }
        ],
        shardKeys: [{x: 1, y: 1}, {x: "hashed", y: 1}],
        metrics: {
            numDocs: 2,
            isUnique: true,
            numDistinctValues: 2,
            mostCommonValues:
                [{value: {x: -1, y: -1}, frequency: 1}, {value: {x: 1, y: 1}, frequency: 1}],
            numMostCommonValues
        }
    });
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
        new ShardingTest({shards: 1, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    runTest(st.s);

    st.stop();
}

{
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    runTest(primary);

    rst.stopSet();
}
})();
