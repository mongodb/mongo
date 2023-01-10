/**
 * Ensure write errors produced by writes without shard keys are propagated to the client.
 *
 * @tags: [
 *    requires_sharding,
 *    requires_fcv_63,
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
const collectionName = "testColl";
const nss = dbName + "." + collectionName;
const testColl = st.getDB(dbName).getCollection(collectionName);
const splitPoint = 0;
const insertDocs = [{_id: 0, x: -2, y: 5}, {_id: 1, x: 2, y: 5}, {_id: 2, x: 3, y: 5}];

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// splitPoint and Shard 1 splitPoint >= 0.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

// Insert initial data.
assert.commandWorked(testColl.insert(insertDocs));

function runCommandAndCheckError(testCase, additionalCmdFields = {}) {
    const cmdObjWithAdditionalFields = Object.assign({}, testCase.cmdObj, additionalCmdFields);
    jsTest.log(cmdObjWithAdditionalFields);

    const res = st.getDB(dbName).runCommand(cmdObjWithAdditionalFields);
    assert.commandFailedWithCode(res, testCase.errorCode);

    // FindAndModify is not a batch command, thus will not have a writeErrors field.
    if (!testCase.cmdObj.findAndModify) {
        res.writeErrors.forEach(writeError => {
            assert(testCase.errorCode.includes(writeError.code));
            assert(testCase.index.includes(writeError.index));
        });
    }
}

const testCases = [
    {
        logMessage: "Unknown modifier in findAndModify, FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {$match: {y: 3}},
        }
    },
    {
        logMessage: "Unknown modifier in batch update, FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        index: [0],
        cmdObj: {
            update: collectionName,
            updates: [{q: {y: 5}, u: {$match: {z: 0}}}],
        }
    },
    {
        logMessage: "Incorrect query in delete, BadValue expected.",
        errorCode: [ErrorCodes.BadValue],
        index: [0],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {y: {$match: 5}}, limit: 1}],
        }
    },
    {
        logMessage: "Two updates in a batch, one successful, one FailedToParse expected.",
        errorCode: [ErrorCodes.FailedToParse],
        index: [0],
        cmdObj: {
            update: collectionName,
            updates: [{q: {y: 5}, u: {$match: {z: 0}}}, {q: {y: 5}, u: {$set: {a: 0}}}],
        }
    },
    {
        logMessage:
            "Three updates in a batch, one BadValue and two FailedToParse expected. If this " +
            "command is run in a transaction, it will abort upon first error.",
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
        logMessage: "Two deletes in a batch, one successful, one BadValue expected.",
        errorCode: [ErrorCodes.BadValue],
        index: [1],
        cmdObj: {
            delete: collectionName,
            deletes: [{q: {y: 5}, limit: 1}, {q: {y: {$match: 5}}, limit: 1}],
        }
    },
    {
        logMessage:
            "Three deletes in a batch, one successful, two BadValues expected. If this command is" +
            " run in a transaction, it will abort upon first error.",
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
    },
];

testCases.forEach(testCase => {
    jsTest.log(testCase.logMessage + "\n" +
               "Running as non-retryable write.");
    runCommandAndCheckError(testCase);

    jsTest.log(testCase.logMessage + "\n" +
               "Running as non-retryable write in a session.");
    const logicalSessionFields = {lsid: {id: UUID()}};
    runCommandAndCheckError(testCase, logicalSessionFields);

    jsTest.log(testCase.logMessage + "\n" +
               "Running as retryable write.");
    const retryableWriteFields = {
        lsid: {id: UUID()},
        txnNumber: NumberLong(0),
    };
    runCommandAndCheckError(testCase, retryableWriteFields);

    jsTest.log(testCase.logMessage + "\n" +
               "Running in a transaction.");
    const transactionFields =
        {lsid: {id: UUID()}, txnNumber: NumberLong(0), startTransaction: true, autocommit: false};
    runCommandAndCheckError(testCase, transactionFields);
});

st.stop();
})();
