// Confirms basic killOp execution via mongos.
// @tags: [requires_replication, requires_sharding]

(function() {
"use strict";

const st = new ShardingTest({shards: 2});
const conn = st.s;

const db = conn.getDB("killOp");
const coll = db.test;
assert.writeOK(db.getCollection(coll.getName()).insert({x: 1}));

const kFailPointName = "waitInFindBeforeMakingBatch";
assert.commandWorked(conn.adminCommand({"configureFailPoint": kFailPointName, "mode": "alwaysOn"}));

const queryToKill = `assert.commandFailedWithCode(db.getSiblingDB('${db.getName()}')` +
    `.runCommand({find: '${coll.getName()}', filter: {x: 1}}), ErrorCodes.Interrupted);`;
const awaitShell = startParallelShell(queryToKill, conn.port);

function runCurOp() {
    const filter = {"ns": coll.getFullName(), "command.filter": {x: 1}};
    return db.getSiblingDB("admin")
        .aggregate([{$currentOp: {localOps: true}}, {$match: filter}])
        .toArray();
}

let opId;

// Wait for the operation to start.
assert.soon(
    function() {
        const result = runCurOp();

        // Check the 'msg' field to be sure that the failpoint has been reached.
        if (result.length === 1 && result[0].msg === kFailPointName) {
            opId = result[0].opid;

            return true;
        }

        return false;
    },
    function() {
        return "Failed to find operation in currentOp() output: " +
            tojson(db.currentOp({"ns": coll.getFullName()}));
    });

// Kill the operation.
assert.commandWorked(db.killOp(opId));

// Ensure that the operation gets marked kill pending while it's still hanging.
let result = runCurOp();
assert(result.length === 1, tojson(result));
assert(result[0].hasOwnProperty("killPending"));
assert.eq(true, result[0].killPending);

// Release the failpoint. The operation should check for interrupt and then finish.
assert.commandWorked(conn.adminCommand({"configureFailPoint": kFailPointName, "mode": "off"}));

awaitShell();

result = runCurOp();
assert(result.length === 0, tojson(result));

st.stop();
})();
