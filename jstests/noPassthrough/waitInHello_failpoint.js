// Tests the shardWaitInHello and routerWaitInHello failpoints.
// @tags: [requires_replication]
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getFailPointName} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(conn, fpName) {
    function runHelloCommand() {
        assert.commandWorked(db.runCommand({hello: 1}));
    }

    // Clear ramlog so checkLog can't find log messages from previous times this fail point was
    // enabled.
    assert.commandWorked(conn.adminCommand({clearLog: 'global'}));

    // Do a find to make sure that the shell has finished running hello while establishing its
    // initial connection.
    assert.eq(0, conn.getDB("test").c.find().itcount());

    // Use a skip of 1, since the parallel shell runs hello when it starts.
    const helloFailpoint = configureFailPoint(conn, fpName, {}, {skip: 1});
    const awaitHello = startParallelShell(runHelloCommand, conn.port);
    helloFailpoint.wait();

    checkLog.contains(conn, "Fail point blocks Hello response until removed");

    helloFailpoint.off();
    awaitHello();
}

const standalone = MongoRunner.runMongod({});
assert.neq(null, standalone, "mongod was unable to start up");
runTest(standalone, getFailPointName("shardWaitInHello", standalone.getMaxWireVersion()));
MongoRunner.stopMongod(standalone);

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
runTest(rst.getPrimary(),
        getFailPointName("shardWaitInHello", rst.getPrimary().getMaxWireVersion()));
rst.stopSet();

const st = new ShardingTest({mongos: 1, shards: [{nodes: 1}], config: 1});
runTest(st.s, getFailPointName("routerWaitInHello", st.s.getMaxWireVersion()));
st.stop();
