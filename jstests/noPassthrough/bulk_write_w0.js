/**
 * Tests bulkWrite command with {w: 0} writeConcern.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   requires_fcv_80,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn) {
    const db = conn.getDB("admin");

    // Run a {w: 0} bulkWrite with 3 operations while setting the return batchSize to 2.
    const res = assert.commandWorked(db.adminCommand({
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {x: 1}},
            {insert: 0, document: {x: 2}},
            {insert: 0, document: {x: 3}},
        ],
        nsInfo: [{ns: "foo.bar"}],
        cursor: {batchSize: 2},
        writeConcern: {w: 0}
    }));
    assert.eq({ok: 1}, res);

    // Test that we skip creating the response cursor for unacknowledged write.
    const bulkWriteCursors =
        db.aggregate([
              {$currentOp: {idleCursors: true}},
              {$match: {"cursor.originatingCommand.bulkWrite": {$exists: true}}}
          ]).toArray();
    assert.eq(0, bulkWriteCursors.length, () => tojson(bulkWriteCursors));
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
