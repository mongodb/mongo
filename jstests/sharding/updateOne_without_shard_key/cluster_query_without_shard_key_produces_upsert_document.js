/**
 * Test success of findAndModify and update commands without shard key, no document matching on the
 * filter and {upsert: true}.
 *
 * @tags: [
 *    requires_sharding,
 *    uses_transactions,
 *    uses_multi_shard_transaction,
 *    featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/sharding/updateOne_without_shard_key/libs/write_without_shard_key_test_util.js");

// 2 shards single node, 1 mongos, 1 config server 3-node
const st = new ShardingTest({});
const dbName = "testDb";
const collectionName = "testColl";
const nss = dbName + "." + collectionName;
const splitPoint = 0;

// Sets up a 2 shard cluster using 'x' as a shard key where Shard 0 owns x <
// the splitpoint and Shard 1 >= splitpoint.
WriteWithoutShardKeyTestUtil.setupShardedCollection(
    st, nss, {x: 1}, [{x: splitPoint}], [{query: {x: splitPoint}, shard: st.shard1.shardName}]);

const testCases = [
    {
        logMessage: "FindAndModify, replacement style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                findAndModify: collectionName,
                query: {a: 0},
                update: {x: -1, y: 7},
                upsert: true,
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'x': -1}, {'y': 7}],
        upsertRequired: true,
    },
    {
        logMessage: "FindAndModify, pipeline style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                findAndModify: collectionName,
                query: {a: 0},
                update: [{$set: {y: 3}}, {$set: {x: 5}}],
                upsert: true,
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'a': 0}, {'x': 5}, {'y': 3}],
        upsertRequired: true,
    },
    {
        logMessage: "FindAndModify, modification style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                findAndModify: collectionName,
                query: {a: -1},
                update: {$inc: {a: 1}},
                upsert: true,
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'a': 0}],
        upsertRequired: true,
    },
    {
        logMessage: "Update, replacement style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{q: {a: 0}, u: {x: -1, y: 7}, upsert: true}],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'x': -1}, {'y': 7}],
        upsertRequired: true,
    },
    {
        logMessage: "Update, pipeline style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{q: {a: 0}, u: [{$set: {y: 3}}, {$set: {x: 5}}], upsert: true}],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'a': 0}, {'x': 5}, {'y': 3}],
        upsertRequired: true,
    },
    {
        logMessage: "Update, modification style update, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{q: {a: 0}, u: {$inc: {a: 1}}, upsert: true}],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{'a': 1}],
        upsertRequired: true,
    },
    {
        logMessage: "Update, arrayFilters, case-insensitive collation, should upsert.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{
                    q: {x: ["bar", "BAR", "foo"]},
                    u: {$set: {'x.$[b]': 'FOO'}},
                    upsert: true,
                    arrayFilters: [{'b': {$eq: 'bar'}}],
                    collation: {locale: 'en_US', strength: 2},
                }],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [{x: ["FOO", "FOO", "foo"]}],
        upsertRequired: true,
    },
    {
        logMessage: "No document matches query, {upsert: false}, no modifications expected.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{q: {a: 0}, u: {$inc: {a: 1}}, upsert: false}],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        expectedMods: [],
        upsertRequired: false,
    },
    {
        logMessage: "Update, incorrect modification style update, {upsert: true}, should error.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                update: collectionName,
                updates: [{q: {a: 0}, u: {$match: {a: 1}}, upsert: true}],
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        errorCode: ErrorCodes.FailedToParse,
    },
    {
        logMessage: "Update immutable _id field, errors.",
        cmdObj: {
            _clusterQueryWithoutShardKey: 1,
            writeCmd: {
                findAndModify: collectionName,
                query: {_id: 0},
                update: {_id: -1, y: 7},
                upsert: true,
            },
            stmtId: NumberInt(0),
            txnNumber: NumberLong(0),
            lsid: {id: UUID()},
            startTransaction: true,
            autocommit: false
        },
        errorCode: ErrorCodes.ImmutableField,
    },
];

testCases.forEach(testCase => {
    jsTest.log(testCase.logMessage + '\n' + tojson(testCase.cmdObj));

    if (testCase.errorCode) {
        assert.commandFailedWithCode(st.getDB(dbName).runCommand(testCase.cmdObj),
                                     testCase.errorCode);
    } else {
        const res = assert.commandWorked(st.getDB(dbName).runCommand(testCase.cmdObj));
        assert.eq(res.upsertRequired, testCase.upsertRequired, res);
        testCase.expectedMods.forEach(mod => {
            let field = Object.keys(mod)[0];
            assert.eq(res.targetDoc[field], mod[field]);
        });

        if (!testCase.upsertRequired) {
            assert.eq(null, res.targetDoc, res.targetDoc);
        }
    }
});

st.stop();
})();
