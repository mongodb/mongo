/**
 * We used to allow setting CWWC with only 'wtimeout,' and it defaulted the 'w' field to 1.
 * Therefore, we issue a warning on startup if a node has CWWC with 'w' field set to 1 and
 * 'wtimeout' is greater than 0.
 * This test ensures that the warning message is indeed issued.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function restartAndCheckLogs(conn, primary, replTest, shouldExist) {
    const includesStartupWarning = (line) => line.includes(
        "The default cluster-wide write concern is configured with the 'w' field set to 1 and" +
        " 'wtimeout' set to a value greater than 0");
    replTest.restart(primary);
    let newPrimaryConn = replTest.getPrimary();
    const startupWarnings =
        assert.commandWorked(newPrimaryConn.adminCommand({getLog: "startupWarnings"}));
    if (shouldExist) {
        assert(startupWarnings.log.some(includesStartupWarning));
    } else {
        assert(!startupWarnings.log.some(includesStartupWarning));
    }

    // In case of replica set.
    if (conn === primary) {
        conn = newPrimaryConn;
    }
    return [conn, newPrimaryConn];
}

function runTest(conn, primary, replTest) {
    jsTestLog("No warning should be issued if CWWC is set to {w: 1, wtimeout: 0}.");
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    let res = conn.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, {"w": 1, "wtimeout": 0});
    [conn, primary] = restartAndCheckLogs(conn, primary, replTest, false);

    jsTestLog("warning should be issued if CWWC is set to {w: 1, wtimeout: 100}.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1, "wtimeout": 100},
        // WC: majority implies journaling.
        writeConcern: {w: "majority"}
    }));
    res = conn.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, {"w": 1, "wtimeout": 100});
    [conn, primary] = restartAndCheckLogs(conn, primary, replTest, true);

    jsTestLog("No warning should be issued if CWWC is set to {w: 1, j: false}.");
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: 1, j: false},
        writeConcern: {w: "majority"}
    }));
    res = conn.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, {"w": 1, "j": false, "wtimeout": 0});
    [conn, primary] = restartAndCheckLogs(conn, primary, replTest, false);
}

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();
runTest(replTest.getPrimary(), replTest.getPrimary(), replTest);
replTest.stopSet();

var st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
});
var mongos = st.s;
var config = st.config0;
runTest(mongos, st.configRS.getPrimary(), st.configRS);
st.stop();
