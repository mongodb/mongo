// Confirms basic killOp execution via mongod and mongos.
// @tags: [
//   requires_replication,
//   requires_sharding,
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "killop";
const collName = "test";

// 'conn' is a connection to either a mongod when testing a replicaset or a mongos when testing
// a sharded cluster. 'shardConn' is a connection to the mongod we enable failpoints on.
function runTest(conn, shardConn) {
    const db = conn.getDB(dbName);

    assert.commandWorked(shardConn.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    assert.commandWorked(shardConn.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "alwaysOn"}));

    const findComment = "unique_find_comment";
    const queryToKill =
        "assert.commandWorked(db.getSiblingDB('" +
        dbName +
        "').runCommand({find: '" +
        collName +
        "', filter: {x: 1}, comment: '" +
        findComment +
        "'}));";
    const awaitShell = startParallelShell(queryToKill, conn.port);
    let opId;

    const curOpFilter = {
        "ns": dbName + "." + collName,
        "command.comment": findComment,
    };
    assert.soon(
        function () {
            const result = db.currentOp(curOpFilter);
            assert.commandWorked(result);
            if (result.inprog.length === 1 && result.inprog[0].numYields > 0) {
                opId = result.inprog[0].opid;
                return true;
            }

            return false;
        },
        function () {
            return (
                "Failed to find operation in currentOp() output: " +
                tojson(db.currentOp({"ns": dbName + "." + collName}))
            );
        },
    );

    assert.commandWorked(db.killOp(opId));

    let result = db.currentOp(curOpFilter);
    assert.commandWorked(result);
    assert(result.inprog.length === 1, tojson(db.currentOp()));
    assert(result.inprog[0].hasOwnProperty("killPending"));
    assert.eq(true, result.inprog[0].killPending);

    assert.commandWorked(shardConn.adminCommand({"configureFailPoint": "setYieldAllLocksHang", "mode": "off"}));

    const exitCode = awaitShell({checkExitSuccess: false});
    assert.neq(0, exitCode, "Expected shell to exit with failure due to operation kill");

    result = db.currentOp(curOpFilter);
    assert.commandWorked(result);
    assert(result.inprog.length === 0, tojson(db.currentOp()));
}

const st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 1});
const shardConn = st.rs0.getPrimary();

// Create the unsharded collection.
assert.commandWorked(st.s.getDB(dbName).getCollection(collName).insert({x: 1}));

// Test killOp against mongod.
runTest(shardConn, shardConn);

// Test killOp against mongos.
runTest(st.s, shardConn);

st.stop();
