/**
 * Test that a user may only override killOp error code if they have the proper privileges.
 *
 * @tags: [requires_fcv_83, requires_sharding]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(m, failPointName) {
    const db = m.getDB("foo");
    const admin = m.getDB("admin");

    admin.createUser({user: "admin", pwd: "password", roles: jsTest.adminUserRoles});
    admin.auth("admin", "password");
    const logReader = {db: "admin", role: "clusterMonitor"};
    db.createUser({user: "reader", pwd: "reader", roles: [{db: "foo", role: "read"}, logReader]});
    admin.createRole({
        role: "opAdmin",
        roles: [],
        privileges: [{resource: {cluster: true}, actions: ["inprog", "killop"]}],
    });
    db.createUser({user: "opAdmin", pwd: "opAdmin", roles: [{role: "opAdmin", db: "admin"}]});

    const t = db.killop_error_code;
    t.insertOne({x: 1});

    if (!FixtureHelpers.isMongos(db)) {
        assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    }

    admin.logout();

    // Only used for nice error messages.
    function getAllLocalOps() {
        return admin.aggregate([{$currentOp: {allUsers: true, localOps: true}}]).toArray();
    }

    function getExpectedOpIds() {
        return admin
            .aggregate([{$currentOp: {localOps: true}}])
            .toArray()
            .filter((op) => op.command.comment === "killop_error_code")
            .map((op) => op.opid);
    }

    let queryAsReader =
        'db = db.getSiblingDB("foo"); db.auth("reader", "reader"); assert.commandFailedWithCode(db.runCommand({find: "killop_error_code", comment: "killop_error_code"}), ErrorCodes.InterruptedDueToOverload);';

    jsTest.log.info("Starting long-running operation");
    db.auth("reader", "reader");
    const failpoint = configureFailPoint(m, failPointName);
    const query = startParallelShell(queryAsReader, m.port);
    jsTest.log.info("Finding ops in $currentOp output");
    assert.soon(
        () => getExpectedOpIds().length === 1,
        () => tojson(getAllLocalOps()),
    );
    const current_op_id = getExpectedOpIds()[0];

    jsTest.log.info("Checking that the user cannot kill the op with a custom error code");
    assert.commandFailedWithCode(
        db.adminCommand({killOp: 1, op: current_op_id, errorCode: ErrorCodes.InterruptedDueToOverload}),
        ErrorCodes.Unauthorized,
    );
    db.logout();

    db.auth("opAdmin", "opAdmin");
    jsTest.log.info("Checking that an administrative user can kill the op only with a valid custom error code");
    assert.commandFailedWithCode(
        db.adminCommand({killOp: 1, op: current_op_id, errorCode: ErrorCodes.DuplicateKey}),
        ErrorCodes.Unauthorized,
    );
    assert.commandWorked(
        db.adminCommand({killOp: 1, op: current_op_id, errorCode: ErrorCodes.InterruptedDueToOverload}),
    );
    db.logout();

    failpoint.off();
    query();
}

let conn = MongoRunner.runMongod({auth: ""});
runTest(conn, "setYieldAllLocksHang");
MongoRunner.stopMongod(conn);

let st = new ShardingTest({shards: 1, keyFile: "jstests/libs/key1"});
runTest(st.s, "waitInFindBeforeMakingBatch");
st.stop();
