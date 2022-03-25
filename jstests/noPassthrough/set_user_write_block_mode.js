// Test setUserWriteBlockMode command.
//
// @tags: [
//   creates_and_authenticates_user,
//   requires_auth,
//   requires_fcv_60,
//   requires_non_retryable_commands,
//   requires_replication,
//   featureFlagUserWriteBlocking,
// ]

(function() {
'use strict';

const WriteBlockState = {
    UNKNOWN: 0,
    DISABLED: 1,
    ENABLED: 2
};

// For this test to work, we expect the state of the collection passed to be a single {a: 2}
// document. This test is expected to maintain that state.
function testCUD(coll, shouldSucceed, expectedFailure) {
    // Ensure we successfully maintained state from last run.
    assert.eq(0, coll.find({a: 1}).count());
    assert.eq(1, coll.find({a: 2}).count());

    if (shouldSucceed) {
        assert.commandWorked(coll.insert({a: 1}));
        assert.eq(1, coll.find({a: 1}).count());
        assert.commandWorked(coll.update({a: 1}, {a: 1, b: 2}));
        assert.eq(1, coll.find({a: 1, b: 2}).count());
        assert.commandWorked(coll.remove({a: 1}));
    } else {
        assert.commandFailedWithCode(coll.insert({a: 1}), expectedFailure);
        assert.commandFailedWithCode(coll.update({a: 2}, {a: 2, b: 2}), expectedFailure);
        assert.eq(0, coll.find({a: 2, b: 2}).count());
        assert.commandFailedWithCode(coll.remove({a: 2}), expectedFailure);
    }

    // Ensure we successfully maintained state on this run.
    assert.eq(0, coll.find({a: 1}).count());
    assert.eq(1, coll.find({a: 2}).count());
}

const bypassUser = "adminUser";
const noBypassUser = "user";
const password = "password";

function runTest(frontend, getStatus, hasReplStatus) {
    const db = frontend.getDB(jsTestName());
    const coll = db.test;
    const admin = frontend.getDB('admin');

    function assertWriteBlockMode(expectedUserWriteBlockMode) {
        const status = getStatus();
        if (hasReplStatus) {
            assert.eq(expectedUserWriteBlockMode, status.repl.userWriteBlockMode);
        } else {
            assert(!("repl" in status));
        }
    }

    function asUser(user, fun) {
        assert(admin.auth(user, password));
        try {
            return fun();
        } finally {
            admin.logout();
        }
    }

    // User with "__system" role has restore role and thus can bypass user write blocking. Can also
    // run setUserWriteBlockMode.
    admin.createUser({user: bypassUser, pwd: password, roles: [{role: "__system", db: "admin"}]});

    asUser(bypassUser, () => {
        // User with "dbAdminAnyDatabase" does not and thus can't bypass. Cannot run
        // setUserWriteBlockMode.
        admin.createUser({
            user: noBypassUser,
            pwd: password,
            roles: [{role: "readWriteAnyDatabase", db: "admin"}]
        });

        // Set up CUD test
        assert.commandWorked(coll.insert({a: 2}));
        // Ensure that without setUserWriteBlockMode, both users are privileged for CUD ops
        assertWriteBlockMode(WriteBlockState.DISABLED);
        testCUD(coll, true);
    });
    asUser(noBypassUser, () => {
        testCUD(coll, true);
        // Ensure that the non-privileged user cannot run setUserWriteBlockMode
        assert.commandFailedWithCode(admin.runCommand({setUserWriteBlockMode: 1, global: true}),
                                     ErrorCodes.Unauthorized);
    });

    asUser(bypassUser, () => {
        assertWriteBlockMode(WriteBlockState.DISABLED);
        // Ensure that privileged user can run setUserWriteBlockMode
        assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: true}));
        assertWriteBlockMode(WriteBlockState.ENABLED);
        // Now with setUserWriteBlockMode enabled, ensure that only the bypassUser can CUD
        testCUD(coll, true);
    });

    asUser(noBypassUser, () => {
        testCUD(coll, false, ErrorCodes.OperationFailed);
    });

    // Now disable userWriteBlockMode and ensure both users can CUD again
    asUser(bypassUser, () => {
        assert.commandWorked(admin.runCommand({setUserWriteBlockMode: 1, global: false}));
        assertWriteBlockMode(WriteBlockState.DISABLED);
        testCUD(coll, true);
    });

    asUser(noBypassUser, () => {
        testCUD(coll, true);
    });
}

// Validate that setting user write blocking fails on standalones
const conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
assert.throws(() => runTest(conn, () => conn.getDB('admin').serverStatus(), false));
MongoRunner.stopMongod(conn);

const keyfile = "jstests/libs/key1";

// Test on replset primary
const rst = new ReplSetTest({nodes: 3, nodeOptions: {auth: "", bind_ip_all: ""}, keyFile: keyfile});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const admin = primary.getDB('admin');
runTest(primary, () => admin.serverStatus(), true);

// Also, test that when we hold a write lock by sleeping in a parallel shell, the userWriteBlockMode
// field in serverStatus will correctly return UNKNOWN and the serverStatus call will not hang.
admin.auth(bypassUser, password);
const parallelShell = startParallelShell(() => {
    db.getSiblingDB('admin').auth("adminUser", "password");
    assert.commandFailedWithCode(db.adminCommand({sleep: 1, lock: "w", seconds: 600}),
                                 ErrorCodes.Interrupted);
}, primary.port);
assert.soon(() => admin.currentOp({"command.sleep": 1, active: true}).inprog.length === 1);

const status = admin.serverStatus();
assert.eq(WriteBlockState.UNKNOWN, status.repl.userWriteBlockMode);

const sleepOps = admin.currentOp({"command.sleep": 1, active: true}).inprog;
assert.eq(sleepOps.length, 1);
const sleepOpId = sleepOps[0].opid;
assert.commandWorked(admin.runCommand({killOp: 1, op: sleepOpId}));
parallelShell();

rst.stopSet();

// Test on a sharded cluster
const st = new ShardingTest({shards: 1, mongos: 1, config: 1, auth: "", other: {keyFile: keyfile}});
const backend = st.rs0.getPrimary();
runTest(st.s,
        () => authutil.asCluster(backend, keyfile, () => backend.getDB('admin').serverStatus()),
        true);
st.stop();
})();
