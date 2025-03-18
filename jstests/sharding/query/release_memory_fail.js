/**
 * Make sure that a releaseMemory failure does not affect the normal query execution.
 *
 * Uses getMore to pin an open cursor.
 * @tags: [
 *   requires_getmore,
 *   requires_fcv_81,
 *   uses_parallel_shell,
 *   # TODO (SERVER-102377): Re-enable this test.
 *   DISABLED_TEMPORARILY_DUE_TO_FCV_UPGRADE,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kFailPointName = "failReleaseMemoryAfterCursorCheckout";
const kFailpointOptions = {
    "errorCode": ErrorCodes.SocketException,
};

const st = new ShardingTest({shards: 2});
const kDBName = "test";
const mongosDB = st.s.getDB(kDBName);
const shard0DB = st.shard0.getDB(kDBName);
const shard1DB = st.shard1.getDB(kDBName);

st.s.adminCommand({enablesharding: kDBName, primaryShard: st.shard0.name});

let coll = mongosDB.jstest_release_memory;

let docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i});
}
assert.commandWorked(coll.insertMany(docs));

st.shardColl(coll, {_id: 1}, {_id: 5}, {_id: 6}, kDBName, false);

function runTest(cursorId, docIdx) {
    // Activate the failpoint and set the exception that it will throw.
    let failpoint = configureFailPoint(mongosDB, kFailPointName, kFailpointOptions);

    // Test releaseMemory
    jsTest.log(`Running releaseMemory command ${tojson({releaseMemory: [cursorId]})}`);
    assert.commandFailedWithCode(
        mongosDB.runCommand({releaseMemory: [cursorId]}),
        ErrorCodes.SocketException,
    );

    // Test getMore
    let batchSize = 4;
    let getMoreCmd = {getMore: cursorId, collection: coll.getName(), batchSize: batchSize};

    while (cursorId != 0) {
        jsTest.log(`Running getMore command ${tojson(getMoreCmd)}`);
        let getMoreRes = mongosDB.runCommand(getMoreCmd);
        assert.commandWorked(getMoreRes);
        let cursor = getMoreRes.cursor;
        assertArrayEq({actual: cursor.nextBatch, expected: docs.slice(docIdx, docIdx + batchSize)});
        docIdx += batchSize;
        cursorId = cursor.id;
    }

    failpoint.off();
}

// Test find command
jsTest.log(`Running find command`);
let findRes = mongosDB.runCommand({find: coll.getName(), sort: {_id: 1}, batchSize: 2});
assert.commandWorked(findRes);
let cursor = findRes.cursor;
assertArrayEq({actual: cursor.firstBatch, expected: docs.slice(0, 2)});
assert.neq(cursor.id, NumberLong(0));
runTest(cursor.id, 2);

// Test aggregate command
jsTest.log(`Running aggregate command`);
let aggregateRes = coll.aggregate([{$sort: {_id: 1}}], {cursor: {batchSize: 2}});
assertArrayEq({actual: aggregateRes._batch, expected: docs.slice(0, 2)});
assert.neq(aggregateRes._cursorid, NumberLong(0));
runTest(aggregateRes._cursorid, 2);

st.stop();
