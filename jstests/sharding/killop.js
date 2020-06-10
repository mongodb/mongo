// Confirms basic killOp execution via mongos.
// @tags: [requires_replication, requires_sharding]

(function() {
"use strict";

load("jstests/libs/curop_helpers.js");  // For waitForCurOpByFailPoint().

const st = new ShardingTest({shards: 2});
const conn = st.s;

const adminDB = conn.getDB("admin");
const db = conn.getDB("killOp");
const coll = db.test;
assert.commandWorked(db.getCollection(coll.getName()).insert({x: 1}));

const kFailPointName = "waitInFindBeforeMakingBatch";
assert.commandWorked(conn.adminCommand({"configureFailPoint": kFailPointName, "mode": "alwaysOn"}));

const queryToKill = `assert.commandFailedWithCode(db.getSiblingDB('${db.getName()}')` +
    `.runCommand({find: '${coll.getName()}', filter: {x: 1}}), ErrorCodes.Interrupted);`;
const awaitShell = startParallelShell(queryToKill, conn.port);

const curOpFilter = {
    ns: coll.getFullName(),
    "command.filter": {x: 1}
};

// Wait for the operation to start.
const curOps = waitForCurOpByFailPointNoNS(db, kFailPointName, curOpFilter, {localOps: true});
const opId = curOps[0].opid;

// Kill the operation.
assert.commandWorked(db.killOp(opId));

// Ensure that the operation gets marked kill pending while it's still hanging.
let result = adminDB.aggregate([{$currentOp: {localOps: true}}, {$match: curOpFilter}]).toArray();
assert(result.length === 1, tojson(result));
assert(result[0].hasOwnProperty("killPending"));
assert.eq(true, result[0].killPending);

// Release the failpoint. The operation should check for interrupt and then finish.
assert.commandWorked(conn.adminCommand({"configureFailPoint": kFailPointName, "mode": "off"}));

awaitShell();

result = adminDB.aggregate([{$currentOp: {localOps: true}}, {$match: curOpFilter}]).toArray();
assert(result.length === 0, tojson(result));

st.stop();
})();
