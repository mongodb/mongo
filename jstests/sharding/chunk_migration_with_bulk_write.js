/**
 * Ensure that chunk migration correctly handles the case of a retryable bulkWrite being run
 * on multiple namespaces (one having been migrated and one not).
 *
 * @tags: [
 *   requires_fcv_80
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {testMoveChunkWithSession} from "jstests/sharding/move_chunk_with_session_helper.js";

const st = new ShardingTest({
    mongos: 1,
    shards: 2,
    rs: {nodes: 3},
    mongosOptions: {setParameter: {featureFlagBulkWriteCommand: true}},
});
const dbName = "test";
const collName = "foo";

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

var cmd = {
    bulkWrite: 1,
    ops: [{insert: 0, document: {_id: 1, x: 100}}, {insert: 1, document: {_id: 0, x: 101}}],
    nsInfo: [{ns: "test.foo"}, {ns: "test.coll"}],
    txnNumber: NumberLong(0)
};

var checkRetryResult = function(result, retryResult) {
    assert.eq(result.ok, retryResult.ok);
    assert.docEq(result.cursor, retryResult.cursor);
    assert.eq(result.numErrors, retryResult.numErrors);
};
var checkDocuments = function(coll) {
    assert.eq(1, coll.findOne({x: 100})._id);
};

testMoveChunkWithSession(
    st, collName, cmd, function(coll) {}, checkRetryResult, checkDocuments, true);

st.stop();
