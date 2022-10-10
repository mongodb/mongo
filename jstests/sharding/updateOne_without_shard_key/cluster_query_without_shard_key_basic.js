/**
 * Basic tests for the internal command _clusterQueryWithoutShardKey. This command is meant to be
 * run as the first phase of the internal updateOne/deleteOne/findAndModify without shard key
 * protocol. The protocol assumes that the collection is sharded and no shard key is given to in the
 * initial request.
 *
 * @tags: [requires_fcv_62, featureFlagUpdateOneWithoutShardKey]
 */
(function() {
"use strict";

let st = new ShardingTest({shards: 2, rs: {nodes: 1}});
let dbName = "test";
let shardedCollNameSingleShard = "shardedCollSingleShard";
let shardedCollNsSingleShard = dbName + "." + shardedCollNameSingleShard;
let shardedCollNameMultiShard = "shardedCollMultiShard";
let unshardedCollName = "unshardedColl";
let shardedCollNsMultiShard = dbName + "." + shardedCollNameMultiShard;
let mongosConn = st.s.getDB(dbName);
let shardConn = st.shard0.getDB(dbName);
let splitPoint = 50;
let xFieldValShard0 = 0;           // monotonically increasing x field value targeting shard0.
let xFieldValShard1 = splitPoint;  // monotonically increasing x field value targeting shard1.
let yFieldVal = 0;                 // monotonically increasing y field value.

// Shard collection
assert.commandWorked(st.s.adminCommand({enablesharding: dbName}));
st.ensurePrimaryShard(dbName, st.shard0.shardName);

// Create a sharded collection sharded on the "x" field and move one of the chunks to another shard.
assert.commandWorked(st.s.adminCommand({shardCollection: shardedCollNsMultiShard, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: shardedCollNsMultiShard, middle: {x: splitPoint}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: shardedCollNsMultiShard, find: {x: splitPoint}, to: st.shard1.shardName}));

// Create a sharded collection sharded on the "x" field and keep both chunks on the same shard.
assert.commandWorked(st.s.adminCommand({shardCollection: shardedCollNsSingleShard, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: shardedCollNsSingleShard, middle: {x: splitPoint}}));

function testCommandFailsOnMongod(testCase) {
    jsTest.log(
        "Test that _clusterQueryWithoutShardKey is not a registered command on a mongod with" +
        " write command: " + tojson(testCase.writeCommand));

    // The command is not sent to the shard in this case, so the transaction is never officially
    // started.
    let lsid = {id: UUID()};
    let txnNumber = NumberLong(1);
    let cmdObj = {
        _clusterQueryWithoutShardKey: 1,
        writeCmd: testCase.writeCommand,
        stmtId: NumberInt(0),
        txnNumber: txnNumber,
        lsid: lsid,
        startTransaction: true,
        autocommit: false
    };
    assert.commandFailedWithCode(shardConn.runCommand(cmdObj), ErrorCodes.CommandNotFound);
}

function testCommandNoMatchingDocument(testCase) {
    jsTest.log("Test that running the command against a collection with no match yields an empty" +
               " document and shard id for write command : " + tojson(testCase.writeCommand));

    let lsid = {id: UUID()};
    let txnNumber = NumberLong(1);
    let cmdObj = {
        _clusterQueryWithoutShardKey: 1,
        writeCmd: testCase.writeCommand,
        stmtId: NumberInt(0),
        txnNumber: txnNumber,
        lsid: lsid,
        startTransaction: true,
        autocommit: false
    };
    let res = assert.commandWorked(mongosConn.runCommand(cmdObj));
    assert.commandWorked(mongosConn.adminCommand(
        {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}));
    assert.eq(res.targetDoc, null);
    assert.eq(res.shardId, null);

    let collectionDocCount = mongosConn.getCollection(shardedCollNameMultiShard).count();
    assert.eq(collectionDocCount, 0);
}

function testCommandUnshardedCollection(testCase) {
    jsTest.log(
        "Test that running the command against an unsharded collection fails with write command: " +
        tojson(testCase.writeCommand));

    // The command is not sent to the shard in this case, so the transaction is never officially
    // started.
    let lsid = {id: UUID()};
    let txnNumber = NumberLong(1);
    let cmdObj = {
        _clusterQueryWithoutShardKey: 1,
        writeCmd: testCase.writeCommand,
        stmtId: NumberInt(0),
        txnNumber: txnNumber,
        lsid: lsid,
        startTransaction: true,
        autocommit: false
    };
    assert.commandFailedWithCode(mongosConn.runCommand(cmdObj), ErrorCodes.InvalidOptions);
}

function testCommandShardedCollectionOnSingleShard(testCase) {
    jsTest.log("Test that running the command against an sharded collection on a single shard" +
               " forwards the request to the primary shard for write command: " + tojson(testCase));
    mongosConn.getCollection(shardedCollNameSingleShard).insert([
        {_id: testCase.shardKeyValShard0, x: testCase.shardKeyValShard0, y: testCase.yFieldVal},
        {_id: testCase.shardKeyValShard1, x: testCase.shardKeyValShard1, y: testCase.yFieldVal}
    ]);

    let doc1Before = mongosConn.getCollection(shardedCollNameSingleShard).findOne({
        _id: testCase.shardKeyValShard0
    });
    let doc2Before = mongosConn.getCollection(shardedCollNameSingleShard).findOne({
        _id: testCase.shardKeyValShard1
    });

    let lsid = {id: UUID()};
    let txnNumber = NumberLong(1);
    let cmdObj = {
        _clusterQueryWithoutShardKey: 1,
        writeCmd: testCase.writeCommand,
        stmtId: NumberInt(0),
        txnNumber: txnNumber,
        lsid: lsid,
        startTransaction: true,
        autocommit: false
    };
    let res = assert.commandWorked(mongosConn.runCommand(cmdObj));
    assert.commandWorked(mongosConn.adminCommand(
        {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}));
    assert.neq(res.targetDoc["_id"], null);

    // Make sure we are targeting the primary shard.
    assert.eq(res.shardId, st.getPrimaryShardIdForDatabase(dbName));

    // Check that no modifications were made to the documents.
    let doc1After = mongosConn.getCollection(shardedCollNameSingleShard).findOne({
        _id: testCase.shardKeyValShard0
    });
    let doc2After = mongosConn.getCollection(shardedCollNameSingleShard).findOne({
        _id: testCase.shardKeyValShard1
    });
    assert.eq(doc1Before, doc1After);
    assert.eq(doc2Before, doc2After);
}

function testCommandShardedCollectionOnMultipleShards(testCase) {
    jsTest.log(
        "Test that running the command against an sharded collection forwards the request to" +
        " all shards and selects a shard that contains a match for write command: " +
        tojson(testCase.writeCommand));

    mongosConn.getCollection(shardedCollNameMultiShard).insert([
        {_id: testCase.shardKeyValShard0, x: testCase.shardKeyValShard0, y: testCase.yFieldVal},
        {_id: testCase.shardKeyValShard1, x: testCase.shardKeyValShard1, y: testCase.yFieldVal}
    ]);

    let doc1Before = mongosConn.getCollection(shardedCollNameMultiShard).findOne({
        _id: testCase.shardKeyValShard0
    });
    let doc2Before = mongosConn.getCollection(shardedCollNameMultiShard).findOne({
        _id: testCase.shardKeyValShard1
    });

    let lsid = {id: UUID()};
    let txnNumber = NumberLong(1);
    let cmdObj = {
        _clusterQueryWithoutShardKey: 1,
        writeCmd: testCase.writeCommand,
        stmtId: NumberInt(0),
        txnNumber: txnNumber,
        lsid: lsid,
        startTransaction: true,
        autocommit: false
    };
    let res = assert.commandWorked(mongosConn.runCommand(cmdObj));
    assert.commandWorked(mongosConn.adminCommand(
        {commitTransaction: 1, lsid: lsid, txnNumber: txnNumber, autocommit: false}));
    assert.neq(res.targetDoc["_id"], null);

    // We can't actually get the shard key from the response since the command projects out only
    // _id, but the way the test is structured, the _id and the shard key have the same value when
    // inserted.
    if (res.targetDoc["_id"] < splitPoint) {
        let hostname = st.shard0.host.split("/")[0];
        assert.eq(res.shardId, hostname);
    } else {
        let hostname = st.shard1.host.split("/")[0];
        assert.eq(res.shardId, hostname);
    }

    // Check that no modifications were made to the documents.
    let doc1After = mongosConn.getCollection(shardedCollNameMultiShard).findOne({
        _id: testCase.shardKeyValShard0
    });
    let doc2After = mongosConn.getCollection(shardedCollNameMultiShard).findOne({
        _id: testCase.shardKeyValShard1
    });
    assert.eq(doc1Before, doc1After);
    assert.eq(doc2Before, doc2After);
}

(() => {
    let testCases = [
        {
            writeCommand: {
                update: unshardedCollName,
                updates: [
                    {q: {x: xFieldValShard0}, u: {$set: {a: 90}}, upsert: false},
                ]
            }
        },
        {
            writeCommand: {
                findAndModify: unshardedCollName,
                query: {x: xFieldValShard0},
                update: {$set: {a: 90}}
            }
        },
        {
            writeCommand: {
                delete: unshardedCollName,
                deletes: [
                    {q: {x: xFieldValShard0}, limit: 1},
                ]
            }
        }
    ];

    testCases.forEach(testCase => {
        testCommandFailsOnMongod(testCase);
    });
})();

(() => {
    let testCases = [
        {
            writeCommand: {
                update: shardedCollNameMultiShard,
                updates: [
                    {q: {x: xFieldValShard0}, u: {$set: {a: 90}}, upsert: false},
                ]
            }
        },
        {
            writeCommand: {
                findAndModify: shardedCollNameMultiShard,
                query: {x: xFieldValShard0},
                update: {$set: {a: 90}}
            }
        },
        {
            writeCommand: {
                delete: shardedCollNameMultiShard,
                deletes: [
                    {q: {x: xFieldValShard0}, limit: 1},
                ]
            }
        }
    ];

    testCases.forEach(testCase => {
        testCommandNoMatchingDocument(testCase);
    });
})();

(() => {
    let testCases = [
        {
            writeCommand: {
                update: unshardedCollName,
                updates: [
                    {q: {x: xFieldValShard0}, u: {$set: {a: 90}}, upsert: false},
                ]
            }
        },
        {
            writeCommand: {
                findandmodify: unshardedCollName,
                query: {x: xFieldValShard0},
                update: {$set: {a: 90}}
            }
        },
        {
            writeCommand: {
                delete: unshardedCollName,
                deletes: [
                    {q: {x: xFieldValShard0}, limit: 1},
                ]
            }
        },
    ];

    testCases.forEach(testCase => {
        testCommandUnshardedCollection(testCase);
    });
})();

(() => {
    let testCases = [
        {
            writeCommand: {
                update: shardedCollNameSingleShard,
                updates: [
                    {q: {y: yFieldVal}, u: {$set: {a: 90}}, upsert: false},
                ]
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
        {
            writeCommand: {
                findandmodify: shardedCollNameSingleShard,
                query: {y: yFieldVal},
                update: {$set: {a: 90}}
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
        {
            writeCommand: {
                delete: shardedCollNameSingleShard,
                deletes: [
                    {q: {y: yFieldVal}, limit: 1},
                ]
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
    ];

    testCases.forEach(testCase => {
        testCommandShardedCollectionOnSingleShard(testCase);
    });
})();

(() => {
    let testCases = [
        {
            writeCommand: {
                update: shardedCollNameMultiShard,
                updates: [
                    {q: {y: yFieldVal}, u: {$set: {a: 90}, upsert: false}},
                ]
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
        {
            writeCommand: {
                findAndModify: shardedCollNameMultiShard,
                query: {y: yFieldVal},
                update: {$set: {a: 90}}
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
        {
            writeCommand: {
                delete: shardedCollNameMultiShard,
                deletes: [
                    {q: {y: yFieldVal}, limit: 1},
                ]
            },
            shardKeyValShard0: xFieldValShard0++,
            shardKeyValShard1: xFieldValShard1++,
            yFieldVal: yFieldVal++
        },
    ];

    testCases.forEach(testCase => {
        testCommandShardedCollectionOnMultipleShards(testCase);
    });
})();

st.stop();
})();
