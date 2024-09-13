/**
 * Tests bulkWrite command shows up in currentop.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_fcv_80,
 *   # The test runs commands that are not allowed with security token: fsyncUnlock.
 *   not_allowed_with_signed_security_token,
 *   assumes_superuser_permissions,
 *   uses_parallel_shell,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const db = conn.getDB("admin");

    const coll = conn.getDB("test").bulkWrite_currentop;
    coll.drop();

    // We fsync+lock the server to cause all subsequent write operations to block.
    assert.commandWorked(conn.getDB("test").fsyncLock());

    const parallelShell = startParallelShell(function() {
        const res = assert.commandWorked(db.adminCommand({
            bulkWrite: 1,
            ops: [
                {insert: 0, document: {x: 1}},
                {insert: 0, document: {x: 2}},
                {insert: 0, document: {x: 3}},
            ],
            nsInfo: [{ns: "test.bulkWrite_currentop"}]
        }));
        assert.commandWorked(res);
    }, conn.port);

    jsTestLog("Checking $currentOp in aggregate");

    assert.soon(function() {
        return conn.getDB("admin")
                   .aggregate([{$currentOp: {localOps: true}}, {$match: {op: "bulkWrite"}}])
                   .toArray()
                   .length == 1;
    }, "currentOp did not find bulkWrite");

    assert.commandWorked(db.fsyncUnlock());
    parallelShell();
}

(function testReplSet() {
    jsTestLog("Running test against a replica set");
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    try {
        runTest(primary);
    } finally {
        rst.stopSet();
    }
})();

(function testSharding() {
    jsTestLog("Running test against a sharded cluster");
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 1});
    try {
        runTest(st.s);
    } finally {
        st.stop();
    }
})();
