/**
 * Test that a user may $currentOp, and then killOp their own operations.
 *
 * Theory of operation: Create a long running operation from a user which does not have the killOp
 * or inProg privileges. Using the same user, run currentOp to get the opId, and then run killOp
 * against it.
 * @tags: [requires_sharding]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(m, failPointName, altFailPointName) {
    let db = m.getDB("foo");
    let admin = m.getDB("admin");

    admin.createUser({user: "admin", pwd: "password", roles: jsTest.adminUserRoles});
    admin.auth("admin", "password");
    const logReader = {db: "admin", role: "clusterMonitor"};
    db.createUser({user: "reader", pwd: "reader", roles: [{db: "foo", role: "read"}, logReader]});
    db.createUser({user: "otherReader", pwd: "otherReader", roles: [{db: "foo", role: "read"}]});
    admin.createRole({
        role: "opAdmin",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["inprog", "killop"]}],
    });
    db.createUser({user: "opAdmin", pwd: "opAdmin", roles: [{role: "opAdmin", db: "admin"}]});

    let t = db.jstests_killop;
    t.save({x: 1});

    if (!FixtureHelpers.isMongos(db)) {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    }

    admin.logout();

    // Only used for nice error messages.
    function getAllLocalOps() {
        admin.aggregate([{$currentOp: {allUsers: true, localOps: true}}]).toArray();
    }

    /**
     * This function filters for the operations that we're looking for, based on their state and
     * the contents of their query object.
     */
    function ops(ownOps = true) {
        const ops = admin.aggregate([{$currentOp: {allUsers: !ownOps, localOps: true}}]).toArray();

        let ids = [];
        for (let o of ops) {
            if (
                (o.active || o.waitingForLock) &&
                o.command &&
                o.command.find === "jstests_killop" &&
                o.command.comment === "kill_own_ops"
            ) {
                ids.push(o.opid);
            }
        }
        return ids;
    }

    let queryAsReader =
        'db = db.getSiblingDB("foo"); db.auth("reader", "reader"); db.jstests_killop.find().comment("kill_own_ops").toArray()';

    jsTestLog("Starting long-running operation");
    db.auth("reader", "reader");
    const fp = configureFailPoint(m, failPointName);
    let s1 = startParallelShell(queryAsReader, m.port);
    jsTestLog("Finding ops in $currentOp output");
    let o = [];
    assert.soon(
        function () {
            o = ops();
            return o.length == 1;
        },
        () => {
            return tojson(getAllLocalOps());
        },
        60000,
    );
    jsTestLog("Checking that another user cannot see or kill the op");
    db.logout();
    db.auth("otherReader", "otherReader");
    assert.eq([], ops());
    assert.commandFailed(db.killOp(o[0]));
    db.logout();
    db.auth("reader", "reader");
    assert.eq(1, ops().length);
    db.logout();
    jsTestLog("Checking that originating user can kill operation");
    let start = new Date();
    db.auth("reader", "reader");
    assert.commandWorked(db.killOp(o[0]));
    checkLog.contains(db, '"msg":"Successful killOp"');
    fp.off();

    jsTestLog("Waiting for ops to terminate");
    let exitCode = s1({checkExitSuccess: false});
    assert.neq(0, exitCode, "expected shell to exit abnormally due to operation execution being terminated");

    // don't want to pass if timeout killed the js function.
    let end = new Date();
    let diff = end - start;
    assert.lt(diff, 30000, "Start: " + start + "; end: " + end + "; diff: " + diff);

    jsTestLog("Starting a second long-running operation");
    const fp2 = configureFailPoint(m, failPointName);
    let s2 = startParallelShell(queryAsReader, m.port);
    jsTestLog("Finding ops in $currentOp output");
    let o2 = [];
    assert.soon(
        function () {
            o2 = ops();
            return o2.length == 1;
        },
        () => {
            return tojson(getAllLocalOps());
        },
        60000,
    );

    db.logout();
    db.auth("opAdmin", "opAdmin");

    jsTestLog("Checking that an administrative user can find others' operations");
    assert.eq(o2, ops(false));

    jsTestLog("Checking that an administrative user cannot find others' operations with ownOps");
    assert.eq([], ops());

    jsTestLog("Checking that an administrative user can kill others' operations");
    start = new Date();
    assert.commandWorked(db.killOp(o2[0]));
    fp2.off();
    jsTestLog("Waiting for ops to terminate");
    exitCode = s2({checkExitSuccess: false});
    assert.neq(0, exitCode, "expected shell to exit abnormally due to JS execution being terminated");

    end = new Date();
    diff = end - start;
    assert.lt(diff, 30000, "Start: " + start + "; end: " + end + "; diff: " + diff);
}

let conn = MongoRunner.runMongod({auth: ""});
runTest(conn, "setYieldAllLocksHang");
MongoRunner.stopMongod(conn);

let st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
// Use a different failpoint in the sharded version, since the mongos does not have a
// setYieldAlllocksHang failpoint.
runTest(st.s, "waitInFindBeforeMakingBatch");
st.stop();
