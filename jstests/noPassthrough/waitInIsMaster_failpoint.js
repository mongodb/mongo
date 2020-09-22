// Tests the waitInHello failpoint.
// @tags: [requires_replication]
(function() {
"use strict";
load("jstests/libs/fail_point_util.js");

function runTest(conn) {
    function runIsMasterCommand() {
        const now = new Date();
        assert.commandWorked(db.runCommand({isMaster: 1}));
        const isMasterDuration = new Date() - now;
        assert.gte(isMasterDuration, 100);
    }

    // Use a skip of 1, since the parallel shell runs isMaster when it starts.
    const helloFailpoint = configureFailPoint(conn, "waitInHello", {}, {skip: 1});
    const awaitIsMaster = startParallelShell(runIsMasterCommand, conn.port);
    helloFailpoint.wait();
    sleep(100);
    helloFailpoint.off();
    awaitIsMaster();
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
