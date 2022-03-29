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

const keyfile = "jstests/libs/key1";

const bypassUser = "adminUser";
const noBypassUser = "user";
const password = "password";

function runTest(fixture) {
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

    // Set up backing collections
    fixture.asUser(({coll}) => assert.commandWorked(coll.insert({a: 2})));

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    // Ensure that without setUserWriteBlockMode, both users are privileged for CUD ops
    fixture.asUser(({coll}) => testCUD(coll, true));
    fixture.asAdmin(({coll}) => testCUD(coll, true));

    fixture.enableWriteBlockMode();

    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    // Now with setUserWriteBlockMode enabled, ensure that only the bypassUser can CUD
    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asUser(({coll}) => {
        testCUD(coll, false, ErrorCodes.OperationFailed);
    });

    // Restarting the cluster has no impact, as write block state is durable
    fixture.restart();

    fixture.assertWriteBlockMode(WriteBlockState.ENABLED);

    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asUser(({coll}) => {
        testCUD(coll, false, ErrorCodes.OperationFailed);
    });

    // Now disable userWriteBlockMode and ensure both users can CUD again

    fixture.disableWriteBlockMode();

    fixture.assertWriteBlockMode(WriteBlockState.DISABLED);

    fixture.asUser(({coll}) => {
        testCUD(coll, true);
    });
    fixture.asAdmin(({coll}) => {
        testCUD(coll, true);
    });

    if (fixture.takeGlobalLock) {
        let globalLock = fixture.takeGlobalLock();
        try {
            fixture.assertWriteBlockMode(WriteBlockState.UNKNOWN);
        } finally {
            globalLock.unlock();
        }
    }
}

// Define a set of classes for managing different cluster types, and allowing the
// test body to perform basic operations against them.
class Fixture {
    // For a localhost node listening on the provided port, create a privileged and unprivileged
    // user. Create a connection authenticated as each user.
    spawnConnections(port) {
        this.adminConn = new Mongo("127.0.0.1:" + port);
        this.adminConn.port = port;
        const admin = this.adminConn.getDB("admin");
        // User with "__system" role has restore role and thus can bypass user write blocking. Can
        // also run setUserWriteBlockMode.
        if (!this.haveCreatedUsers) {
            assert.commandWorked(admin.runCommand(
                {createUser: bypassUser, pwd: password, roles: [{role: "__system", db: "admin"}]}));
        }
        assert(admin.auth(bypassUser, password));

        if (!this.haveCreatedUsers) {
            assert.commandWorked(admin.runCommand({
                createUser: noBypassUser,
                pwd: password,
                roles: [{role: "readWriteAnyDatabase", db: "admin"}]
            }));
            this.haveCreatedUsers = true;
        }

        this.conn = new Mongo("127.0.0.1:" + port);
        this.conn.port = port;
        assert(this.conn.getDB("admin").auth(noBypassUser, password));
    }

    constructor(port) {
        this.spawnConnections(port);
    }

    asUser(fun) {
        const db = this.conn.getDB(jsTestName());
        const coll = db.test;
        return fun({conn: this.conn, db: db, coll: coll});
    }

    asAdmin(fun) {
        const db = this.adminConn.getDB(jsTestName());
        const coll = db.test;
        return fun({conn: this.adminConn, db: db, coll: coll});
    }

    enableWriteBlockMode() {
        assert.commandWorked(
            this.adminConn.getDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}));
    }

    disableWriteBlockMode() {
        assert.commandWorked(
            this.adminConn.getDB("admin").runCommand({setUserWriteBlockMode: 1, global: false}));
    }

    getStatus() {
        throw "UNIMPLEMENTED";
    }

    assertWriteBlockMode(expectedUserWriteBlockMode) {
        const status = this.getStatus();
        assert.eq(expectedUserWriteBlockMode, status.repl.userWriteBlockMode);
    }

    restart() {
        throw "UNIMPLEMENTED";
    }

    stop() {
        throw "UNIMPLEMENTED";
    }
}

class ReplicaFixture extends Fixture {
    constructor() {
        const rst =
            new ReplSetTest({nodes: 3, nodeOptions: {auth: "", bind_ip_all: ""}, keyFile: keyfile});
        rst.startSet();
        rst.initiate();

        super(rst.getPrimary().port);
        this.rst = rst;
    }

    getStatus() {
        return this.adminConn.getDB("admin").serverStatus();
    }

    takeGlobalLock() {
        load("jstests/libs/parallel_shell_helpers.js");

        class LockHolder {
            constructor(fixture, waiter, opId) {
                this.fixture = fixture;
                this.waiter = waiter;
                this.opId = opId;
            }

            unlock() {
                assert.commandWorked(
                    this.fixture.adminConn.getDB("admin").runCommand({killOp: 1, op: this.opId}));
                this.waiter();
            }
        }

        const parallelShell = startParallelShell(
            funWithArgs((connString, username, password) => {
                let admin = db.getSiblingDB("admin");
                admin.auth(username, password);
                assert.commandFailedWithCode(admin.runCommand({sleep: 1, lock: "w", secs: 600}),
                                             ErrorCodes.Interrupted);
            }, "127.0.0.1:" + this.conn.port, bypassUser, password), this.conn.port);

        var opId;

        assert.soon(() => {
            let result = this.adminConn.getDB("admin")
                             .aggregate([
                                 {$currentOp: {}},
                                 {$match: {op: "command", "command.sleep": {$exists: true}}}
                             ])
                             .toArray();
            if (result.length !== 1)
                return false;

            opId = result[0].opid;

            return true;
        });

        return new LockHolder(this, parallelShell, opId);
    }

    restart() {
        this.rst.stopSet(undefined, /* restart */ true);
        this.rst.startSet({}, /* restart */ true);
        this.rst.waitForPrimary();

        super.spawnConnections(this.rst.getPrimary().port);
    }

    stop() {
        this.rst.stopSet();
    }
}

{
    // Validate that setting user write blocking fails on standalones
    const conn = MongoRunner.runMongod({auth: "", bind_ip: "127.0.0.1"});
    const admin = conn.getDB("admin");
    assert.commandWorked(admin.runCommand(
        {createUser: "root", pwd: "root", roles: [{role: "__system", db: "admin"}]}));
    assert(admin.auth("root", "root"));

    assert.commandFailedWithCode(admin.runCommand({setUserWriteBlockMode: 1, global: true}),
                                 ErrorCodes.IllegalOperation);
    MongoRunner.stopMongod(conn);
}

// Test on replset primary
const rst = new ReplicaFixture();
runTest(rst);
rst.stop();

class ShardingFixture extends Fixture {
    constructor() {
        const st = new ShardingTest(
            {shards: 1, mongos: 1, config: 1, auth: "", other: {keyFile: keyfile}});

        super(st.s.port);
        this.st = st;
    }

    getStatus() {
        const backend = this.st.rs0.getPrimary();
        return authutil.asCluster(backend, keyfile, () => backend.getDB('admin').serverStatus());
    }

    restart() {
        this.st.restartShardRS(0);
        this.st.restartConfigServer(0);
    }

    stop() {
        this.st.stop();
    }
}

// Test on a sharded cluster
const st = new ShardingFixture();
runTest(st);
st.stop();
})();
