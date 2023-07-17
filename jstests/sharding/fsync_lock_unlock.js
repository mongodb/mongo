/**
 * Verifies the fsync with lock+unlock command on mongos.
 * @tags: [
 *   requires_fsync,
 *   featureFlagClusterFsyncLock,
 *   uses_parallel_shell,
 * ]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "collTest";
const ns = dbName + "." + collName;
const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagClusterFsyncLock: true}},
    config: 1
});
const adminDB = st.s.getDB('admin');

function waitUntilOpCountIs(opFilter, num, st) {
    assert.soon(() => {
        let ops = st.s.getDB('admin')
                      .aggregate([
                          {$currentOp: {}},
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

jsTest.log("Insert some data.");
const coll = st.s0.getDB(dbName)[collName];
assert.commandWorked(coll.insert({x: 1}));

// unlock before lock should fail
let ret = assert.commandFailed(st.s.adminCommand({fsyncUnlock: 1}));
const errmsg = "fsyncUnlock called when not locked";
assert.eq(ret.errmsg.includes(errmsg), true);

// lock then unlock
assert.commandWorked(st.s.adminCommand({fsync: 1, lock: true}));

// Make sure writes are blocked. Spawn a write operation in a separate shell and make sure it
// is blocked. There is really no way to do that currently, so just check that the write didn't
// go through.
let codeToRun = () => {
    assert.commandWorked(db.getSiblingDB("test").getCollection("collTest").insert({x: 1}));
};

let writeOpHandle = startParallelShell(codeToRun, st.s.port);

waitUntilOpCountIs({op: 'insert', ns: 'test.collTest', waitingForLock: true}, 1, st);

// Make sure reads can still run even though there is a pending write and also that the write
// didn't get through.
assert.eq(1, coll.find({}).itcount());
assert.commandWorked(st.s.adminCommand({fsyncUnlock: 1}));

writeOpHandle();

// ensure writers are allowed after the cluster is unlocked
assert.commandWorked(coll.insert({x: 1}));
assert.eq(coll.count(), 3);

st.stop();
}());
