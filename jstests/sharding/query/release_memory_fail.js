/**
 * Make sure that a releaseMemory failure does not affect the normal query execution.
 *
 * Uses getMore to pin an open cursor.
 * @tags: [
 *   requires_getmore,
 *   requires_fcv_82,
 *   uses_parallel_shell,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {assertReleaseMemoryFailedWithCode} from "jstests/libs/release_memory_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kFailPointName = "failReleaseMemoryAfterCursorCheckout";
const kFailpointOptions = {
    "errorCode": ErrorCodes.SocketException,
    "failInternalCommands": true,
};

const st = new ShardingTest({shards: 2, nodes: 1});
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

function runTest(cursorId, docIdx, failpointConn) {
    // Activate the failpoint and set the exception that it will throw.
    let failpoint = configureFailPoint(failpointConn, kFailPointName, kFailpointOptions);

    // Test releaseMemory
    jsTest.log(`Running releaseMemory command ${tojson({releaseMemory: [cursorId]})}`);
    const res = mongosDB.runCommand({releaseMemory: [cursorId]});
    assertReleaseMemoryFailedWithCode(res, cursorId, ErrorCodes.SocketException);

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

// Test failure propagation by setting up fail points on either shards or the router.
for (let failpointConn of [mongosDB, shard0DB, shard1DB]) {
    let findRes = mongosDB.runCommand({find: coll.getName(), sort: {_id: 1}, batchSize: 2});
    assert.commandWorked(findRes);
    let cursor = findRes.cursor;
    assertArrayEq({actual: cursor.firstBatch, expected: docs.slice(0, 2)});
    assert.neq(cursor.id, NumberLong(0));
    runTest(cursor.id, 2, failpointConn);
}

st.stop();
