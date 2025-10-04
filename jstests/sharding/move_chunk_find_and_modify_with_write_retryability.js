import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testMoveChunkWithSession} from "jstests/sharding/move_chunk_with_session_helper.js";

let checkFindAndModifyResult = function (expected, toCheck) {
    assert.eq(expected.ok, toCheck.ok);
    assert.eq(expected.value, toCheck.value);
    assert.eq(expected.lastErrorObject, toCheck.lastErrorObject);
};

let lsid = UUID();
let tests = [
    {
        coll: "findAndMod-upsert",
        cmd: {
            findAndModify: "findAndMod-upsert",
            query: {x: 60},
            update: {$inc: {y: 1}},
            new: true,
            upsert: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(37),
        },
        setup: function (coll) {},
        checkRetryResult: function (result, retryResult) {
            checkFindAndModifyResult(result, retryResult);
        },
        checkDocuments: function (coll) {
            assert.eq(1, coll.findOne({x: 60}).y);
        },
    },
    {
        coll: "findAndMod-update-preImage",
        cmd: {
            findAndModify: "findAndMod-update-preImage",
            query: {x: 60},
            update: {$inc: {y: 1}},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(38),
        },
        setup: function (coll) {
            coll.insert({x: 60});
        },
        checkRetryResult: function (result, retryResult) {
            checkFindAndModifyResult(result, retryResult);
        },
        checkDocuments: function (coll) {
            assert.eq(1, coll.findOne({x: 60}).y);
        },
    },
    {
        coll: "findAndMod-update-postImage",
        cmd: {
            findAndModify: "findAndMod-update-postImage",
            query: {x: 60},
            update: {$inc: {y: 1}},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(39),
        },
        setup: function (coll) {
            coll.insert({x: 60});
        },
        checkRetryResult: function (result, retryResult) {
            checkFindAndModifyResult(result, retryResult);
        },
        checkDocuments: function (coll) {
            assert.eq(1, coll.findOne({x: 60}).y);
        },
    },
    {
        coll: "findAndMod-delete",
        cmd: {
            findAndModify: "findAndMod-delete",
            query: {x: 10},
            remove: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(40),
        },
        setup: function (coll) {
            let bulk = coll.initializeUnorderedBulkOp();
            for (let i = 0; i < 10; i++) {
                bulk.insert({x: 10});
            }
            assert.commandWorked(bulk.execute());
        },
        checkRetryResult: function (result, retryResult) {
            checkFindAndModifyResult(result, retryResult);
        },
        checkDocuments: function (coll) {
            assert.eq(9, coll.find({x: 10}).itcount());
        },
    },
];

// Prevent unnecessary elections in the first shard replica set. Shard 'rs1' shard will need its
// secondary to get elected, so we don't give it a zero priority.
let st = new ShardingTest({
    mongos: 2,
    shards: {
        rs0: {nodes: [{rsConfig: {}}, {rsConfig: {priority: 0}}]},
        rs1: {nodes: [{rsConfig: {}}, {rsConfig: {}}]},
    },
});
assert.commandWorked(st.s.adminCommand({enableSharding: "test", primaryShard: st.shard0.shardName}));

tests.forEach(function (test) {
    testMoveChunkWithSession(st, test.coll, test.cmd, test.setup, test.checkRetryResult, test.checkDocuments);
});

st.stop();
