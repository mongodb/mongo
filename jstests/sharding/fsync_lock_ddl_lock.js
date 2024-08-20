/**
 * This test script
 * - Runs a DDL operation and waits before the DDL op takes a lock, calls fsync with lock: true, and
 * verifies that the DDL op can take the lock after fsyncUnlock call.
 *
 * @tags: [
 *   requires_fsync,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
"use strict";
const dbName = "test";
const collName = "collTest";
const renamedCollName = "collTest1";
const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
const db = st.s0.getDB(dbName);
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
const coll = st.s.getDB(dbName).getCollection(collName);
coll.insert({x: 1});
assert.eq(coll.count(), 1);

function waitUntilOpCountIs(opFilter, num, st) {
    assert.soon(() => {
        let ops = st.s.getDB('admin')
                      .aggregate([
                          {$currentOp: {allUsers: true}},
                          {$match: opFilter},
                      ])
                      .toArray();
        if (ops.length != num) {
            jsTest.log("Num operations: " + ops.length + ", expected: " + num);
            jsTest.log(ops);
            return false;
        }
        return true;
    });
}

let ddlCoordinatorFailPoint =
    configureFailPoint(st.getPrimaryShard(dbName), 'hangBeforeRunningCoordinatorInstance');

let codeToRun = () => {
    const collName = "collTest";
    let sourceNss = db[collName].getFullName();
    let destNss = sourceNss + "1";
    assert.commandWorked(db.adminCommand({renameCollection: sourceNss, to: destNss}));
};

let ddlOpHandle = startParallelShell(codeToRun, st.s.port);

ddlCoordinatorFailPoint.wait();

assert.commandWorked(st.s.adminCommand({fsync: 1, lock: true}));

waitUntilOpCountIs({desc: 'RenameCollectionCoordinator'}, 1, st);

ddlCoordinatorFailPoint.off();
assert.commandWorked(st.s.adminCommand({fsyncUnlock: 1}));

ddlOpHandle();
assert.commandWorked(db[renamedCollName].insert({x: 2}));
assert.eq(db[renamedCollName].count(), 2);
st.stop();
}());
