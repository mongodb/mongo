/**
 * Test writes without shard key uses the appropriate query plan.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_71,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// Make sure we're testing with no implicit session.
TestData.disableImplicitSessions = true;

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collName = "testColl";
const nss = dbName + "." + collName;
const splitPoint = 0;
const docsToInsert = [
    {_id: 0, x: -2, y: 1, z: [1, 2, 3]},
    {_id: 1, x: -1, y: 1, z: [1, 2, 3]},
    {_id: 2, x: 1, y: 1, z: [1, 2, 3]},
    {_id: 3, x: 2, y: 1, z: [1, 2, 3]},
    {_id: 4, x: 3, y: 1, z: [1, 2, 3]},
    {_id: 5, x: 4, y: 1, z: [1, 2, 3]}
];
const dbConn = st.s.getDB(dbName);
const profileCollectionShard0 = st.shard0.getDB(dbName).system.profile;
const profileCollectionShard1 = st.shard1.getDB(dbName).system.profile;

function runTest(testCase, usingClusteredIndex) {
    // Turn on profiling for both shards.
    assert.commandWorked(st.shard0.getDB(dbName).setProfilingLevel(2));
    assert.commandWorked(st.shard1.getDB(dbName).setProfilingLevel(2));

    assert.commandWorked(dbConn.runCommand(testCase.cmdObj));

    const profileOnShard0 = profileCollectionShard0.findOne(testCase.profileDocToFind);
    const profileOnShard1 = profileCollectionShard1.findOne(testCase.profileDocToFind);

    // Only one shard should have recorded that it performed the write.
    assert.neq(profileOnShard0, profileOnShard1);
    const profileDoc = profileOnShard0 ? profileOnShard0 : profileOnShard1;

    if (usingClusteredIndex) {
        if (testCase.hasPositionalProjection) {
            assert.eq(profileDoc.execStats.stage, "PROJECTION_DEFAULT", profileDoc);
            assert.eq(
                profileDoc.execStats.inputStage.inputStage.stage, "CLUSTERED_IXSCAN", profileDoc);
        } else {
            assert.eq(profileDoc.execStats.inputStage.stage, "CLUSTERED_IXSCAN", profileDoc);
        }
    } else {
        // The two phase write protocol will include the original query and collation for updates
        // with a positional operator, which means it cannot only use the _id index to fulfill the
        // query.
        if (testCase.hasPositionalUpdate) {
            assert.eq(profileDoc.execStats.inputStage.stage, "FETCH", profileDoc);
            assert.eq(profileDoc.execStats.inputStage.inputStage.stage, "IXSCAN", profileDoc);
        } else if (testCase.hasPositionalProjection) {
            assert.eq(profileDoc.execStats.stage, "PROJECTION_DEFAULT", profileDoc);
            assert.eq(profileDoc.execStats.inputStage.inputStage.stage, "FETCH", profileDoc);
            assert.eq(
                profileDoc.execStats.inputStage.inputStage.inputStage.stage, "IXSCAN", profileDoc);
        } else {
            assert.eq(profileDoc.execStats.inputStage.stage, "IDHACK", profileDoc);
        }
    }

    // Turn off profiling on both shards so we can clear the systems.profile collection for the next
    // test.
    assert.commandWorked(st.shard0.getDB(dbName).setProfilingLevel(0));
    assert.commandWorked(st.shard1.getDB(dbName).setProfilingLevel(0));
    profileCollectionShard0.drop();
    profileCollectionShard1.drop();
}

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

assert.commandWorked(dbConn.getCollection(collName).insert(docsToInsert));

// There should only be one collection created in this test.
const listCollRes = assert.commandWorked(dbConn.runCommand({listCollections: 1}));
const usingClusteredIndex = listCollRes.cursor.firstBatch[0].options.clusteredIndex != null;

let testCases = [
    {
        logMessage: "Running updateOne without positional update.",
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1}, u: {$set: {a: 3}}}],
        },
        profileDocToFind: {"op": "update", "ns": nss}
    },
    {
        logMessage: "Running updateOne without positional update and non-default collation.",
        cmdObj: {
            update: collName,
            updates: [
                {q: {y: 1}, u: {$set: {a: 3}}, collation: {locale: "en", strength: 2}},
            ],
        },
        profileDocToFind: {"op": "update", "ns": nss}
    },
    {
        logMessage: "Running updateOne with positional update.",
        cmdObj: {
            update: collName,
            updates: [{q: {y: 1, z: 1}, u: {$set: {"z.$": 3}}}],
        },
        hasPositionalUpdate: true,
        profileDocToFind: {"op": "update", "ns": nss}
    },
    {
        logMessage: "Running updateOne with positional update and non-default collation.",
        cmdObj: {
            update: collName,
            updates:
                [{q: {y: 1, z: 1}, u: {$set: {"z.$": 3}}, collation: {locale: "en", strength: 2}}],
        },
        hasPositionalUpdate: true,
        profileDocToFind: {"op": "update", "ns": nss}
    },
    {
        logMessage: "Running findAndModify update without positional update.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {a: 4}},
        },
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage:
            "Running findAndModify update without positional update and non-default collation.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            update: {$set: {a: 4}},
            collation: {locale: "en", strength: 2}
        },
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running findAndModify update with positional update.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1, z: 1},
            update: {$set: {"z.$": 3}},
        },
        hasPositionalUpdate: true,
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage:
            "Running findAndModify update with positional update and non-default collation.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1, z: 1},
            update: {$set: {"z.$": 3}},
            collation: {locale: "en", strength: 2}
        },
        hasPositionalUpdate: true,
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running findAndModify with positional projection.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1, z: 1},
            fields: {'z.$': 1},
            remove: true,
        },
        hasPositionalProjection: true,
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running findAndModify with positional projection and non-default collation.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1, z: 1},
            fields: {'z.$': 1},
            update: {$set: {a: 3}},
            collation: {locale: "en", strength: 2}

        },
        hasPositionalProjection: true,
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running findAndModify remove.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            remove: true,
        },
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running findAndModify remove and non-default collation.",
        cmdObj: {
            findAndModify: collName,
            query: {y: 1},
            collation: {locale: "en", strength: 2},
            remove: true,
        },
        profileDocToFind: {"op": "command", "ns": nss, "command.findAndModify": collName}
    },
    {
        logMessage: "Running deleteOne.",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1}],
        },
        profileDocToFind: {"op": "remove", "ns": nss}
    },
    {
        logMessage: "Running deleteOne and non-default collation.",
        docsToInsert: docsToInsert,
        cmdObj: {
            delete: collName,
            deletes: [{q: {y: 1}, limit: 1, collation: {locale: "en", strength: 2}}],
        },
        profileDocToFind: {"op": "remove", "ns": nss}
    }
];

testCases.forEach(testCase => {
    jsTestLog(testCase.logMessage);
    runTest(testCase, usingClusteredIndex);
});

st.stop();
})();
