/**
 * Tests that verify metrics counters related to updateOne, deleteOne, and findAndModify commands
 * are correctly incremented.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_70,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collectionName = "testColl";
const ns = dbName + "." + collectionName;
const testColl = st.getDB(dbName).getCollection(collectionName);
const unshardedCollName = "unshardedColl";
const unshardedColl = st.getDB(dbName).getCollection(unshardedCollName);

const splitPoint = 0;

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 x >= splitPoint.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, ns, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

function runCommandAndVerify(testCase) {
    testCase.insertDocs.forEach(function(insertDoc) {
        assert.commandWorked(testCase.collName.insert(insertDoc));
    });
    const res = st.getDB(dbName).runCommand(testCase.cmdObj);
    assert.commandWorked(res);

    if (testCase.resultDocs) {
        testCase.resultDocs.forEach(function(resultDoc) {
            assert.eq(resultDoc, testCase.collName.findOne(resultDoc));
        });
    } else {
        // Check if all the inserted docs were deleted in the delete commands.
        testCase.insertDocs.forEach(function(insertDoc) {
            assert.eq(null, testCase.collName.findOne(insertDoc));
        });
    }
}

function runCommandAndCheckError(testCase) {
    const res = st.getDB(dbName).runCommand(testCase.cmdObj);
    assert.commandFailedWithCode(res, testCase.errorCode);

    // FindAndModify is not a batch command, thus will not have a writeErrors field.
    if (!testCase.cmdObj.findAndModify) {
        res.writeErrors.forEach(writeError => {
            assert(testCase.errorCode.includes(writeError.code));
            assert(testCase.index.includes(writeError.index));
        });
    }
}

let mongosServerStatus = st.s.getDB(dbName).adminCommand({serverStatus: 1});

// Verify all all counter metrics are 0 before executing write commands.
assert.eq(0, mongosServerStatus.metrics.query.updateOneTargetedShardedCount);
assert.eq(0, mongosServerStatus.metrics.query.deleteOneTargetedShardedCount);
assert.eq(0, mongosServerStatus.metrics.query.findAndModifyTargetedShardedCount);
assert.eq(0, mongosServerStatus.metrics.query.updateOneUnshardedCount);
assert.eq(0, mongosServerStatus.metrics.query.deleteOneUnshardedCount);
assert.eq(0, mongosServerStatus.metrics.query.findAndModifyUnshardedCount);
assert.eq(0, mongosServerStatus.metrics.query.updateOneNonTargetedShardedCount);
assert.eq(0, mongosServerStatus.metrics.query.deleteOneNonTargetedShardedCount);
assert.eq(0, mongosServerStatus.metrics.query.findAndModifyNonTargetedShardedCount);

const testCases = [
    {
        // This will increase updateOneNonTargetedShardedCount by 1.
        logMessage:
            "Running non-targeted updateOne command on sharded collection without shard key.",
        collName: testColl,
        insertDocs: [{_id: 0, x: 0, a: 0}],
        resultDocs: [{_id: 0, x: 0, a: 5}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {a: 0}, u: {$inc: {a: 5}}}],
        }
    },
    {
        // This will increase updateOneTargetedShardedCount by 1.
        logMessage:
            "Running targeted updateOne command on sharded collection without shard key but _id is specified.",
        collName: testColl,
        insertDocs: [{_id: 1, x: 1}],
        resultDocs: [{_id: 1, x: 1, b: 1}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {_id: 1}, u: {$set: {b: 1}}}],
        }
    },
    {
        // This will increase updateOneTargetedShardedCount by 1.
        logMessage: "Running targeted updateOne command on sharded collection with shard key.",
        collName: testColl,
        insertDocs: [{_id: 2, x: -1}],
        resultDocs: [{_id: 2, x: -1, c: 2}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {x: -1}, u: {$set: {c: 2}}}],
        }
    },
    {
        // This will increase updateOneNonTargetedShardedCount by 2 since there are two updates.
        logMessage:
            "Running non-targeted updateOne command with multiple updates on sharded collection without shard key.",
        collName: testColl,
        insertDocs: [{_id: 3, x: 2, d: -5}, {_id: 4, x: -2, d: 5}],
        resultDocs: [{_id: 3, x: 2, d: -5, a: 1}, {_id: 4, x: -2, d: 5, a: 2}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {d: -5}, u: {$set: {a: 1}}}, {q: {d: 5}, u: {$set: {a: 2}}}]
        }
    },
    {
        // This will increase deleteOneNonTargetedShardedCount by 1.
        logMessage:
            "Running non-targeted deleteOne command on sharded collection without shard key.",
        collName: testColl,
        insertDocs: [{_id: 5, x: 3, y: 0}],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {y: 0}, limit: 1}],
        }
    },
    {
        // This will increase deleteOneTargetedShardedCount by 1.
        logMessage:
            "Running targeted deleteOne command on sharded collection without shard key but _id is specified.",
        collName: testColl,
        insertDocs: [{_id: 6, x: -3}],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {_id: 6}, limit: 1}],
        }
    },
    {
        // This will increase deleteOneTargetedShardedCount by 1.
        logMessage: "Running targeted deleteOne command on sharded collection with shard key.",
        collName: testColl,
        insertDocs: [{_id: 7, x: 4}],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {x: 4}, limit: 1}],
        }
    },
    {
        // This will increase deleteOneNonTargetedShardedCount by 2 since there are two deletes.
        logMessage:
            "Running non-targeted deleteOne commmand with multiple deletes on sharded collection without shard key.",
        collName: testColl,
        insertDocs: [{_id: 8, x: -4, y: 8}, {_id: 9, x: 5, y: 9}],
        cmdObj: {delete: collectionName, deletes: [{q: {y: 8}, limit: 1}, {q: {y: 9}, limit: 1}]}
    },
    {
        // This will increase findAndModifyNonTargetedShardedCount by 1.
        logMessage:
            "Running non-targeted findAndModify command on sharded collection without shard key.",
        collName: testColl,
        insertDocs: [{_id: 10, x: -5, e: 5}],
        resultDocs: [{_id: 10, x: -5, e: 10, f: 15}],
        cmdObj: {
            findAndModify: collectionName,
            query: {e: 5},
            update: {_id: 10, x: -5, e: 10, f: 15},
        }
    },
    {
        // This will increase findAndModifyTargetedShardedCount by 1.
        logMessage: "Running targeted findAndModify command on sharded collection with shard key.",
        collName: testColl,
        insertDocs: [{_id: 11, x: 6, f: 0}],
        resultDocs: [{_id: 11, x: 6, f: 5}],
        cmdObj: {findAndModify: collectionName, query: {x: 6}, update: {_id: 11, x: 6, f: 5}}
    },
    {
        // This will increase the updateOneUnshardedCount by 1.
        logMessage: "Running targeted updateOne command on unsharded collection.",
        collName: unshardedColl,
        insertDocs: [{_id: 12, x: -6}],
        resultDocs: [{_id: 12, x: -6, g: 20}],
        cmdObj: {
            update: unshardedCollName,
            updates: [{q: {_id: 12}, u: {$set: {g: 20}}}],
        }
    },
    {
        // This will increase the deleteOneUnshardedCount by 1.
        logMessage: "Running targeted deleteOne commmand on unsharded collection.",
        collName: unshardedColl,
        insertDocs: [{_id: 13, x: 7}],
        cmdObj: {
            delete: unshardedCollName,
            deletes: [{q: {_id: 13}, limit: 1}],
        }
    },
    {
        // This will increase findAndModifyUnshardedCount by 1.
        logMessage: "Running findAndModify command on unsharded collection.",
        collName: unshardedColl,
        insertDocs: [{_id: 14, x: -7, h: 0}],
        resultDocs: [{_id: 14, x: -7, h: 25}],
        cmdObj:
            {findAndModify: unshardedCollName, query: {_id: 14}, update: {_id: 14, x: -7, h: 25}}
    },
    {
        // This will increase updateOneNonTargetedShardedCount by 1.
        logMessage:
            "Running a single update where no document matches on the query and {upsert: true}",
        collName: testColl,
        insertDocs: [],
        resultDocs: [{_id: 50, x: -50}],
        cmdObj:
            {update: collectionName, updates: [{q: {k: 50}, u: {_id: 50, x: -50}, upsert: true}]}
    },
    {
        // This will increase updateOneTargetedShardedCount by 1.
        logMessage:
            "Running a single update where no document matches on the query and {upsert: true} with shard key",
        collName: testColl,
        insertDocs: [],
        resultDocs: [{_id: 51, x: 51, k: 51}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {x: 51}, u: {_id: 51, x: 51, k: 51}, upsert: true}]
        }
    }
];

testCases.forEach(testCase => {
    jsTest.log(testCase.logMessage);
    runCommandAndVerify(testCase);
});

mongosServerStatus = st.s.getDB(dbName).adminCommand({serverStatus: 1});

// Verify all counter metrics were updated correctly after the write commands.
assert.eq(3, mongosServerStatus.metrics.query.updateOneTargetedShardedCount);
assert.eq(2, mongosServerStatus.metrics.query.deleteOneTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.updateOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.deleteOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyUnshardedCount);
assert.eq(4, mongosServerStatus.metrics.query.updateOneNonTargetedShardedCount);
assert.eq(3, mongosServerStatus.metrics.query.deleteOneNonTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyNonTargetedShardedCount);

// Testing the counters with WCOS commands.

const WCOStestCases = [
    {
        // This call will increase deleteOneTargetedShardedCount by 1 and
        // updateOneNonTargetedShardedCount by 2.
        logMessage: "Running non-targeted WouldChangeOwningShard updateOne command.",
        collName: testColl,
        insertDocs: [{_id: 15, x: 8, y: 1}],
        resultDocs: [{_id: 15, x: -8, y: 1}],
        cmdObj: {
            update: collectionName,
            updates: [{q: {y: 1}, u: {x: -8, y: 1}}],
            lsid: {id: UUID()},
            txnNumber: NumberLong(1)
        }
    },
    {
        // This call will increase deleteOneTargetedShardedCount by 1 and
        // findAndModifyNonTargetedShardedCount by 2.
        logMessage: "Running non-targeted WouldChangeOwningShard findAndModify command.",
        collName: testColl,
        insertDocs: [{_id: 16, x: 9, z: 1}],
        resultDocs: [{_id: 16, x: -9, z: 1}],
        cmdObj: {
            findAndModify: collectionName,
            query: {_id: 16},
            update: {_id: 16, x: -9, z: 1},
            lsid: {id: UUID()},
            txnNumber: NumberLong(1)
        }
    }
];

WCOStestCases.forEach(testCase => {
    jsTest.log(testCase.logMessage);
    runCommandAndVerify(testCase);
});

mongosServerStatus = st.s.getDB(dbName).adminCommand({serverStatus: 1});

// Verify all counter metrics were updated correctly after the wcos write commands.
assert.eq(3, mongosServerStatus.metrics.query.updateOneTargetedShardedCount);
assert.eq(4, mongosServerStatus.metrics.query.deleteOneTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.updateOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.deleteOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyUnshardedCount);
assert.eq(6, mongosServerStatus.metrics.query.updateOneNonTargetedShardedCount);
assert.eq(3, mongosServerStatus.metrics.query.deleteOneNonTargetedShardedCount);
assert.eq(3, mongosServerStatus.metrics.query.findAndModifyNonTargetedShardedCount);

// Insert Docs for error testing.
const insertDocs = [{_id: 17, x: 10, y: 5}, {_id: 18, x: -10, y: 5}, {_id: 19, x: 11, y: 5}];
assert.commandWorked(testColl.insert(insertDocs));

const errorTestCases = [
    {
        // This call will increase findAndModifyNonTargetedShardedCount by 1 even though the
        // command should fail.
        logMessage: "Unknown modifier in findAndModify, FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {$match: {y: 3}},
        }
    },
    {
        // This call will updateOneNonTargetedShardedCount by 1.
        logMessage: "Unknown modifier in batch update, FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        index: [0],
        cmdObj: {
            update: collectionName,
            updates: [{q: {y: 5}, u: {$match: {z: 0}}}],
        }
    },
    {
        // This call will not increase any counters because query had invalid operator.
        logMessage: "Incorrect query in delete, BadValue expected.",
        errorCode: [ErrorCodes.BadValue],
        index: [0],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {y: {$match: 5}}, limit: 1}],
        }
    },
    {
        // This call will updateOneNonTargetedShardedCount by 2.
        logMessage: "Two updates in a batch, one successful, one FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        index: [1],
        cmdObj: {
            update: collectionName,
            updates: [{q: {y: 5}, u: {$set: {a: 0}}}, {q: {y: 5}, u: {$match: {z: 0}}}],
        }
    },
    {
        // This call will increase updateOneNonTargetedShardedCount by 2.
        logMessage: "Three updates in a batch, one BadValue and two FailedToParse expected.",
        errorCode: [ErrorCodes.BadValue, ErrorCodes.FailedToParse],
        index: [0, 1, 2],
        cmdObj: {
            update: collectionName,
            updates: [
                {q: {y: {$match: 5}}, u: {$set: {z: 0}}},
                {q: {y: 5}, u: {$match: {z: 0}}},
                {q: {y: 5}, u: {$match: {z: 0}}}
            ],
            ordered: false
        }
    },
    {
        // This call will increase deleteOneNonTargetedShardedCount by 1.
        logMessage: "Two deletes in a batch, one successful, one BadValue expected.",
        errorCode: [ErrorCodes.BadValue],
        index: [1],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {y: 5}, limit: 1}, {q: {y: {$match: 5}}, limit: 1}],
        }
    },
    {
        // This call will increase deleteOneNonTargetedShardedCount by 1.
        logMessage: "Three deletes in a batch, one successful, two BadValues expected.",
        errorCode: [ErrorCodes.BadValue],
        index: [0, 2],
        cmdObj: {
            delete: collectionName,
            deletes: [
                {q: {y: {$match: 5}}, limit: 1},
                {q: {y: 5}, limit: 1},
                {q: {y: {$match: 5}}, limit: 1}
            ],
            ordered: false
        }
    }
];

errorTestCases.forEach(testCase => {
    jsTest.log(testCase.logMessage);
    runCommandAndCheckError(testCase);
});

mongosServerStatus = st.s.getDB(dbName).adminCommand({serverStatus: 1});

// Verify all counter metrics were not updated after the error write commands.
assert.eq(3, mongosServerStatus.metrics.query.updateOneTargetedShardedCount);
assert.eq(4, mongosServerStatus.metrics.query.deleteOneTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyTargetedShardedCount);
assert.eq(1, mongosServerStatus.metrics.query.updateOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.deleteOneUnshardedCount);
assert.eq(1, mongosServerStatus.metrics.query.findAndModifyUnshardedCount);
assert.eq(11, mongosServerStatus.metrics.query.updateOneNonTargetedShardedCount);
assert.eq(5, mongosServerStatus.metrics.query.deleteOneNonTargetedShardedCount);
assert.eq(4, mongosServerStatus.metrics.query.findAndModifyNonTargetedShardedCount);

st.stop();
})();
