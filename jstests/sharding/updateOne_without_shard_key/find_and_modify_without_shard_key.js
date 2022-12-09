/**
 * Test success of findAndModify without shard key command with various query filters,
 * on a sharded collection.
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

load("jstests/sharding/libs/sharded_transactions_helpers.js");

// 2 shards single node, 1 mongos, 1 config server 3-node.
const st = new ShardingTest({});
const dbName = "testDb";
const collectionName = "testColl";
const ns = dbName + "." + collectionName;
const testColl = st.getDB(dbName).getCollection(collectionName);

// Set up.
// shard0 -- x: (-inf, 0)
// shard1 -- x: [0, inf)
assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

function verifyResult(testCase, res) {
    if (testCase.errorCode) {
        assert.commandFailedWithCode(res, testCase.errorCode);
    } else {
        assert.commandWorked(res);
        assert.eq(testCase.resultDoc, testColl.findOne(testCase.resultDoc));

        // No document matched the query, no modification was made.
        if (testCase.insertDoc.y != testCase.cmdObj.query.y) {
            assert.eq(0, res.lastErrorObject.n, res);
            assert.eq(false, res.lastErrorObject.updatedExisting);
        } else {
            assert.eq(1, res.lastErrorObject.n, res);

            // Check for pre/post image in command response.
            if (testCase.cmdObj.new) {
                assert.eq(testCase.resultDoc, res.value, res.value);
            } else {
                assert.eq(testCase.insertDoc, res.value, res.value);
            }
        }
    }

    // Clean up, remove document from db.
    assert.commandWorked(testColl.deleteOne({_id: testCase.insertDoc._id}));
    assert.eq(null, testColl.findOne({_id: testCase.insertDoc._id}));
}

// When more than one document matches the command query, ensure that a single document is modified
// and that the remaining documents are unchanged.
function verifySingleModification(testCase, res) {
    var modifiedDocId;
    var modifiedDoc;
    if (testCase.errorCode) {
        assert.commandFailedWithCode(res, testCase.errorCode);
        modifiedDocId = -1;  // No document should be modified, none will match on -1.
    } else {
        assert.commandWorked(res);

        // No document matched the query.
        if (testCase.insertDoc[0].y != testCase.cmdObj.query.y) {
            assert.eq(0, res.lastErrorObject.n, res);
            assert.eq(false, res.lastErrorObject.updatedExisting);

            modifiedDocId = -1;  // No document should be modified, none will match on -1.
        } else {
            assert.eq(1, res.lastErrorObject.n, res);

            // If this findAndModify removes a document, it must be found using the _id value from
            // the response image. Otherwise, we can query on the non-null result doc.
            modifiedDocId = res.value._id;
            const query = testCase.resultDoc ? testCase.resultDoc : {_id: modifiedDocId};
            modifiedDoc = testColl.findOne(query);
        }
    }

    testCase.insertDoc.forEach(doc => {
        if (doc._id == modifiedDocId) {
            // This is the document that got modified. Check for pre/post image in command response.
            if (testCase.cmdObj.new) {
                assert.eq(modifiedDoc, res.value, res.value);
            } else {
                assert.eq(doc, res.value, res.value);
            }
        } else {
            // Confirm that the original document exists in the db.
            assert.eq(doc, testColl.findOne({_id: doc._id}));
        }

        // Clean up, remove document from db.
        assert.commandWorked(testColl.deleteOne({_id: doc._id}));
        assert.eq(null, testColl.findOne({_id: doc._id}));
    });
}

function runCommandAndVerify(testCase, additionalCmdFields = {}) {
    const cmdObjWithAdditionalFields = Object.assign({}, testCase.cmdObj, additionalCmdFields);

    assert.commandWorked(testColl.insert(testCase.insertDoc));
    const res = st.getDB(dbName).runCommand(cmdObjWithAdditionalFields);

    if (cmdObjWithAdditionalFields.hasOwnProperty("autocommit") && !testCase.errorCode) {
        assert.commandWorked(st.s.getDB(dbName).adminCommand({
            commitTransaction: 1,
            lsid: cmdObjWithAdditionalFields.lsid,
            txnNumber: cmdObjWithAdditionalFields.txnNumber,
            autocommit: false
        }));
    }

    if (testCase.insertDoc.length > 1) {
        return verifySingleModification(testCase, res);
    } else {
        verifyResult(testCase, res);
    }
}

const testCases = [
    {
        logMessage: "Replacement update style, no sort filter, post image.",
        insertDoc: {_id: 0, x: -1, y: 5},
        resultDoc: {_id: 0, x: -1, y: 7},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {_id: 0, x: -1, y: 7},
            new: true,
        }
    },
    {
        logMessage: "Aggregation update style, no sort filter, preimage.",
        insertDoc: {_id: 1, x: 1, y: 4},
        resultDoc: {_id: 1, x: 1, y: 1},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 4},
            update: [{$set: {y: 0}}, {$set: {y: 1}}],
        }
    },
    {
        logMessage: "Modification style update, no sort filter, preimage.",
        insertDoc: {_id: 2, x: -2, y: 6},
        resultDoc: {_id: 2, x: -2, y: 9},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 6},
            update: {$inc: {y: 3}},
        }
    },
    {
        logMessage: "Query does not match, no update.",
        insertDoc: {_id: 2, x: -2, y: 6},
        resultDoc: {_id: 2, x: -2, y: 6},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {$inc: {y: 3}},
        }
    },
    {
        logMessage: "Remove, no sort filter, preimage.",
        insertDoc: {_id: 3, x: -2, y: 5},
        resultDoc: null,
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            remove: true,
        }
    },
    {
        logMessage:
            "Insert two documents matching on the query, one on each shard, ensure only one is updated (modification).",
        insertDoc: [{_id: 0, x: -2, y: 5}, {_id: 1, x: 2, y: 5}],
        resultDoc: {y: 8},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {$inc: {y: 3}},
            new: true,
        }
    },
    {
        logMessage:
            "Insert two documents matching on the query, one on each shard, ensure only one is updated (aggregation).",
        insertDoc: [{_id: 0, x: -2, y: 5}, {_id: 1, x: 2, y: 5}],
        resultDoc: {y: 1},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: [{$set: {y: 0}}, {$set: {y: 1}}],
            new: true,
        }
    },
    {
        logMessage:
            "Insert two documents matching on the query, one on each shard, ensure only one is updated (replacement).",
        insertDoc: [{_id: 0, x: -2, y: 5}, {_id: 1, x: 2, y: 5}],
        resultDoc: {y: 8},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 5},
            update: {$inc: {y: 3}},
            new: true,
        }
    },
    {
        logMessage:
            "Insert two documents matching on the query, one on each shard, ensure only one is removed.",
        insertDoc: [
            {_id: 0, x: -2, y: 3},
            {_id: 1, x: 2, y: 3},
        ],
        resultDoc: null,
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 3},
            remove: true,
        }
    },
    {
        logMessage:
            "Insert two documents, one on each shard, ensure neither is modified when query does not match.",
        insertDoc: [{_id: 0, x: -2, y: 5}, {_id: 1, x: 2, y: 5}],
        resultDoc: {y: 5},
        cmdObj: {
            findAndModify: collectionName,
            query: {y: 4},
            update: {$inc: {y: 3}},
        }
    },
];

jsTest.log("Testing findAndModify without a shard key commands in various configurations.");
testCases.forEach(testCase => {
    jsTest.log(testCase.logMessage);
    runCommandAndVerify(testCase);

    const logicalSessionFields = {lsid: {id: UUID()}};
    runCommandAndVerify(testCase, logicalSessionFields);

    const retryableWriteFields = {
        lsid: {id: UUID()},
        txnNumber: NumberLong(0),
        stmtId: NumberInt(1)
    };
    runCommandAndVerify(testCase, retryableWriteFields);

    const transactionFields =
        {lsid: {id: UUID()}, txnNumber: NumberLong(0), startTransaction: true, autocommit: false};
    runCommandAndVerify(testCase, transactionFields);
});

st.stop();
})();
