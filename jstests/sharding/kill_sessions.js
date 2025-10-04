/*
 * Run the kill_sessions tests against a sharded cluster.
 */

import {KillSessionsTestHelper} from "jstests/libs/kill_sessions.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test involves killing all sessions, which will not work as expected if the kill command is
// sent with an implicit session.
TestData.disableImplicitSessions = true;

function runTests(needAuth) {
    let other = {
        rs: true,
        rs0: {nodes: 3},
        rs1: {nodes: 3},
    };
    if (needAuth) {
        other.keyFile = "jstests/libs/key1";
    }

    let st = new ShardingTest({shards: 2, mongos: 1, other: other});

    let forExec = st.s0;

    if (needAuth) {
        KillSessionsTestHelper.initializeAuth(forExec);
    }

    let forKill = new Mongo(forExec.host);

    let r = forExec.getDB("admin").runCommand({
        multicast: {ping: 1},
        db: "admin",
    });
    assert(r.ok);

    let hosts = [];
    for (var host in r["hosts"]) {
        var host = new Mongo(host);
        if (needAuth) {
            host.getDB("local").auth("__system", "foopdedoop");
        }
        hosts.push(host);
    }

    let args = [forExec, forKill, hosts];
    if (needAuth) {
        KillSessionsTestHelper.runAuth.apply({}, args);
    } else {
        KillSessionsTestHelper.runNoAuth.apply({}, args);
    }

    st.stop();
}

runTests(true);
runTests(false);
