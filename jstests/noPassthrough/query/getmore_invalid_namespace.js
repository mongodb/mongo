// A get more exception caused by an invalid or unauthorized get more request does not cause
// the get more's ClientCursor to be destroyed.  This prevents an unauthorized user from
// improperly killing a cursor by issuing an invalid get more request.
// This test also makes sure that mongod and mongos behave the same in case getMore is called with
// an invalid namespace (see SERVER-102285).

import {ShardingTest} from "jstests/libs/shardingtest.js";

function performTest(conn) {
    const dbName = 'getmore_cmd_test';
    const collName = 'getmore_cmd_invalid_namespace';
    const db = conn.getDB(dbName);
    const coll = db[collName];
    coll.drop();

    const numDocs = 10;
    const initialBatchSize = 2;

    assert.commandWorked(db.createCollection(collName));
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(coll.insert({a: i}));
    }

    let cmdRes;
    // Run a find against the initial collection to establish a cursor.
    cmdRes = db.runCommand({find: collName, batchSize: initialBatchSize});
    jsTestLog(cmdRes);
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.firstBatch.length, initialBatchSize);
    for (let i = 0; i < cmdRes.cursor.firstBatch.length; i++) {
        assert.eq(cmdRes.cursor.firstBatch[i]["a"], i);
    }

    let cursorId = cmdRes.cursor.id;
    assert.neq(cursorId, 0);

    // Now attempt a getMore with an invalid namespace.
    cmdRes = db.runCommand({getMore: cursorId, collection: "invalid_namespace_for_getMore_test"});
    jsTestLog(cmdRes);
    assert.commandFailedWithCode(cmdRes, ErrorCodes.Unauthorized);

    // Now use the cursor again on a valid namespace to get the rest of the data.
    cmdRes = db.runCommand({getMore: cursorId, collection: collName});
    jsTestLog(cmdRes);
    assert.commandWorked(cmdRes);
    assert.eq(cmdRes.cursor.id, 0);
    assert.eq(cmdRes.cursor.nextBatch.length, numDocs - initialBatchSize);
    for (let i = 0; i < cmdRes.cursor.nextBatch.length; i++) {
        assert.eq(cmdRes.cursor.nextBatch[i]["a"], i + initialBatchSize);
    }
}

const conn = MongoRunner.runMongod({});
performTest(conn);
MongoRunner.stopMongod(conn);

let st = new ShardingTest({mongos: 1, shards: 1});
performTest(st);
st.stop();
