// Tests the waitInIsMaster failpoint.
// @tags: [requires_replication]
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

function runTest(conn) {
    function runHelloCommand() {
        const now = new Date();
        assert.commandWorked(db.runCommand({hello: 1}));
        const helloDuration = new Date() - now;
        assert.gte(helloDuration, 100);
    }

    // Do a find to make sure that the shell has finished running hello while establishing its
    // initial connection.
    assert.eq(0, conn.getDB("test").c.find().itcount());

    // Use a skip of 1, since the parallel shell runs hello when it starts.
    const isMasterFailpoint = configureFailPoint(conn, "waitInIsMaster", {}, {skip: 1});
    const awaitHello = startParallelShell(runHelloCommand, conn.port);
    isMasterFailpoint.wait();
    sleep(100);
    isMasterFailpoint.off();
    awaitHello();
}

const standalone = MongoRunner.runMongod({});
assert.neq(null, standalone, "mongod was unable to start up");
runTest(standalone);
MongoRunner.stopMongod(standalone);

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
runTest(rst.getPrimary());
rst.stopSet();

const st = new ShardingTest({mongos: 1, shards: [{nodes: 1}], config: 1});
runTest(st.s);
st.stop();
}());
