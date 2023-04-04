/**
 * Tests the internal command _clusterWriteWithoutShardKey. The command must be run in a
 * transaction.
 *
 * @tags: [requires_fcv_63, featureFlagUpdateOneWithoutShardKey]
 */
(function() {
"use strict";

let st = new ShardingTest({shards: 2, rs: {nodes: 1}});
let dbName = "test";
let collName = "foo";
let ns = dbName + "." + collName;
let mongosConn = st.s.getDB(dbName);
let shardConn = st.shard0.getDB(dbName);
let shard0Name = st.shard0.shardName;
const testColl = mongosConn.getCollection(collName);
let _id = 0;
const aFieldValue = 50;  // Arbitrary field value used for testing.
let xFieldValueShard0 =
    -1;  // Shard key value to place document on shard0, which has chunks x:(-inf, 0).
let xFieldValueShard1 =
    0;  // Shard key value to place document on shard1, which has chunks x: [0,inf).
let unshardedCollName = "unshardedCollection";

// Shard collection
assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, shard0Name);

// Create a sharded collection and move the chunk x:[0,inf) to shard1.
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

function runAndVerifyCommand(testCase) {
    testColl.insert(testCase.docToInsert);

    // Run and commit transaction.
    let res = assert.commandWorked(mongosConn.runCommand(testCase.cmdObj));
    assert.commandWorked(mongosConn.adminCommand({
        commitTransaction: 1,
        lsid: testCase.cmdObj.lsid,
        txnNumber: testCase.cmdObj.txnNumber,
        autocommit: false
    }));

    switch (Object.keys(testCase.cmdObj.writeCmd)[0]) {
        case "update":
            assert.eq(1, res.response.nModified, res.response);
            assert.eq(res.shardId, testCase.cmdObj.shardId);
            assert.eq(testCase.expectedResultDoc,
                      mongosConn.getCollection(collName).findOne(testCase.expectedResultDoc));

            // Remove document.
            assert.commandWorked(testColl.deleteOne(testCase.cmdObj.targetDocId));
            assert.eq(null,
                      mongosConn.getCollection(collName).findOne(testCase.cmdObj.targetDocId));
            break;
        case "delete":
            assert.eq(1, res.response.n, res.response);
            assert.eq(res.shardId, testCase.cmdObj.shardId);
            assert.eq(null,
                      mongosConn.getCollection(collName).findOne(testCase.cmdObj.targetDocId));
            break;
        case "findAndModify":
        case "findandmodify":
            assert.eq(1, res.response.lastErrorObject.n, res.response);
            assert.eq(res.shardId, testCase.cmdObj.shardId);
            assert.eq(testCase.expectedModifiedDoc,
                      mongosConn.getCollection(collName).findOne(testCase.cmdObj.targetDocId));

            // Check for pre/post image in command response.
            if (testCase.cmdObj.writeCmd.new) {
                assert.eq(res.response.value, testCase.expectedModifiedDoc);
            } else {
                assert.eq(res.response.value, testCase.docToInsert);
            }

            // Remove document.
            assert.commandWorked(testColl.deleteOne(testCase.cmdObj.targetDocId));
            assert.eq(null,
                      mongosConn.getCollection(collName).findOne(testCase.cmdObj.targetDocId));
            break;
    }
}

(() => {
    jsTest.log("Testing success of update and delete commands.");

    let testCases = [
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    update: collName,
                    updates: [
                        {
                            q: {},
                            u: {$set: {a: aFieldValue}},
                            collation: {locale: "simple"},
                        },
                    ],
                    writeConcern: {w: "majority"},
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedResultDoc: {_id: _id, x: xFieldValueShard0, a: aFieldValue}
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard1, a: aFieldValue},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    update: collName,
                    updates: [
                        // Testing modification style updates.
                        {q: {}, u: {$inc: {a: 1}}},
                    ],
                },
                shardId: st.shard1.shardName,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedResultDoc: {_id: _id, x: xFieldValueShard1, a: aFieldValue + 1}
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard1, a: aFieldValue},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    update: collName,
                    updates: [
                        {
                            q: {},
                            // Testing replacement style updates.
                            u: {_id: _id, x: xFieldValueShard1, a: aFieldValue + 2}
                        },
                    ],
                },
                shardId: st.shard1.shardName,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedResultDoc: {_id: _id, x: xFieldValueShard1, a: aFieldValue + 2}
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard1, a: 0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    update: collName,
                    updates: [
                        {
                            q: {},
                            // Testing aggregation pipeline style updates.
                            u: [{$set: {a: aFieldValue}}, {$set: {a: 5}}]
                        },
                    ],
                },
                shardId: st.shard1.shardName,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedResultDoc: {_id: _id, x: xFieldValueShard1, a: 5}
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    delete: collName,
                    deletes: [{
                        q: {},
                        limit: 1,
                    }],
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            }
        },
    ];

    testCases.forEach(testCase => {
        jsTest.log(tojson(testCase));
        runAndVerifyCommand(testCase);
    });
})();

(() => {
    jsTest.log("Testing of pre and post image findAndModify commands.");

    let testCases = [
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    findAndModify: collName,
                    query: {},
                    update: [
                        {$set: {a: aFieldValue}},
                    ]
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedModifiedDoc: {_id: _id, x: xFieldValueShard0, a: aFieldValue},
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    findAndModify: collName,
                    query: {},
                    update: [
                        {$set: {a: aFieldValue}},
                    ],
                    // Testing post-image.
                    new: true
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedModifiedDoc: {_id: _id, x: xFieldValueShard0, a: aFieldValue},
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    findandmodify: collName,
                    query: {},
                    update: [
                        {$set: {a: aFieldValue}},
                    ]
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedModifiedDoc: {_id: _id, x: xFieldValueShard0, a: aFieldValue},
        },
        {
            docToInsert: {_id: _id, x: xFieldValueShard0},
            cmdObj: {
                _clusterWriteWithoutShardKey: 1,
                writeCmd: {
                    findandmodify: collName,
                    query: {},
                    // Testing removal.
                    remove: true,
                },
                shardId: shard0Name,
                targetDocId: {_id: _id},
                txnNumber: NumberLong(1),
                lsid: {id: UUID()},
                startTransaction: true,
                autocommit: false
            },
            expectedModifiedDoc: null,
        },
    ];

    testCases.forEach(testCase => {
        jsTest.log(tojson(testCase));
        runAndVerifyCommand(testCase);
    });

    // Ensure that only 1 document was updated in last findandmodify command even though two
    // documents on the same shard match the command query.
    testColl.insert({_id: 0, x: xFieldValueShard0, a: 0});
    let confirmOnlyOneDocUpdatedObj = {
        docToInsert: {_id: 1, x: xFieldValueShard0, a: 0},
        cmdObj: {
            _clusterWriteWithoutShardKey: 1,
            writeCmd: {
                findandmodify: collName,
                query: {a: 0},
                update: [
                    {$set: {a: aFieldValue}},
                ]
            },
            shardId: shard0Name,
            targetDocId: {_id: 1},
            txnNumber: NumberLong(1),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedModifiedDoc: {_id: 1, x: xFieldValueShard0, a: aFieldValue},
    };
    runAndVerifyCommand(confirmOnlyOneDocUpdatedObj);
    assert.eq({_id: 0, x: xFieldValueShard0, a: 0},
              mongosConn.getCollection(collName).findOne({_id: 0}));
})();

(() => {
    jsTest.log("Testing error cases.");
    testColl.insert([{_id: _id, x: xFieldValueShard0}]);

    // Test that the command is not registered on a shard.
    let cmdObj = {
        _clusterWriteWithoutShardKey: 1,
        writeCmd: {insert: collName, documents: [{_id: _id, x: xFieldValueShard0}]},
        shardId: shard0Name,
        targetDocId: {_id: _id},
        txnNumber: NumberLong(1),
        lsid: {id: UUID()},
        startTransaction: true,
        autocommit: false
    };
    assert.commandFailedWithCode(shardConn.runCommand(cmdObj), ErrorCodes.CommandNotFound);

    // Must run on a sharded collection.
    cmdObj = {
        _clusterWriteWithoutShardKey: 1,
        writeCmd: {
            update: unshardedCollName,
            updates: [
                {q: {}, u: {$set: {a: 90}}},
            ],
        },
        shardId: shard0Name,
        targetDocId: {_id: _id},
        txnNumber: NumberLong(1),
        lsid: {id: UUID()},
        startTransaction: true,
        autocommit: false
    };
    mongosConn.getCollection(unshardedCollName).insert([{_id: _id, a: aFieldValue}]);
    assert.commandFailedWithCode(mongosConn.runCommand(cmdObj), ErrorCodes.InvalidOptions);

    // Must run in a transaction.
    cmdObj = {
        _clusterWriteWithoutShardKey: 1,
        writeCmd: {insert: collName, documents: [{_id: _id, x: xFieldValueShard0}]},
        shardId: shard0Name,
        targetDocId: {_id: _id},
    };
    assert.commandFailedWithCode(mongosConn.runCommand(cmdObj), ErrorCodes.IllegalOperation);
})();

st.stop();
})();
