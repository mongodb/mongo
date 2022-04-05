load("jstests/libs/fail_point_util.js");
load("jstests/libs/parallel_shell_helpers.js");

const UserWriteBlockHelpers = (function() {
    'use strict';

    const WriteBlockState = {UNKNOWN: 0, DISABLED: 1, ENABLED: 2};

    const bypassUser = "adminUser";
    const noBypassUser = "user";
    const password = "password";

    const keyfile = "jstests/libs/key1";

    // Define a set of classes for managing different cluster types, and allowing the
    // test body to perform basic operations against them.
    class Fixture {
        // For a localhost node listening on the provided port, create a privileged and unprivileged
        // user. Create a connection authenticated as each user.
        spawnConnections(port) {
            this.adminConn = new Mongo("127.0.0.1:" + port);
            this.adminConn.port = port;
            const admin = this.adminConn.getDB("admin");
            // User with "__system" role has restore role and thus can bypass user write blocking.
            // Can also run setUserWriteBlockMode.
            if (!this.haveCreatedUsers) {
                assert.commandWorked(admin.runCommand({
                    createUser: bypassUser,
                    pwd: password,
                    roles: [{role: "__system", db: "admin"}]
                }));
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
            return fun({conn: this.adminConn, db: db, admin: db.getSiblingDB("admin"), coll: coll});
        }

        enableWriteBlockMode() {
            assert.commandWorked(
                this.adminConn.getDB("admin").runCommand({setUserWriteBlockMode: 1, global: true}));
        }

        disableWriteBlockMode() {
            assert.commandWorked(this.adminConn.getDB("admin").runCommand(
                {setUserWriteBlockMode: 1, global: false}));
        }

        getStatus() {
            throw "UNIMPLEMENTED";
        }

        assertWriteBlockMode(expectedUserWriteBlockMode) {
            const status = this.getStatus();
            assert.eq(expectedUserWriteBlockMode, status.repl.userWriteBlockMode);
        }

        _hangTransition(targetConn, failpoint) {
            let hangWhileSetingUserWriteBlockModeFailPoint =
                configureFailPoint(targetConn, failpoint);

            const awaitShell = startParallelShell(
                funWithArgs((username, password) => {
                    let admin = db.getSiblingDB("admin");
                    admin.auth(username, password);
                    assert.commandWorked(
                        admin.runCommand({setUserWriteBlockMode: 1, global: true}));
                }, bypassUser, password), this.conn.port);

            hangWhileSetingUserWriteBlockModeFailPoint.wait();

            return {waiter: awaitShell, failpoint: hangWhileSetingUserWriteBlockModeFailPoint};
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
            const rst = new ReplSetTest(
                {nodes: 3, nodeOptions: {auth: "", bind_ip_all: ""}, keyFile: keyfile});
            rst.startSet();
            rst.initiate();

            super(rst.getPrimary().port);
            this.rst = rst;
        }

        getStatus() {
            return this.adminConn.getDB("admin").serverStatus();
        }

        takeGlobalLock() {
            class LockHolder {
                constructor(fixture, waiter, opId) {
                    this.fixture = fixture;
                    this.waiter = waiter;
                    this.opId = opId;
                }

                unlock() {
                    assert.commandWorked(this.fixture.adminConn.getDB("admin").runCommand(
                        {killOp: 1, op: this.opId}));
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

    class ShardingFixture extends Fixture {
        constructor() {
            const st = new ShardingTest(
                {shards: 1, mongos: 1, config: 1, auth: "", other: {keyFile: keyfile}});

            super(st.s.port);
            this.st = st;
        }

        getStatus() {
            const backend = this.st.rs0.getPrimary();
            return authutil.asCluster(
                backend, keyfile, () => backend.getDB('admin').serverStatus());
        }

        hangTransition() {
            return this._hangTransition(this.st.shard0, "hangInShardsvrSetUserWriteBlockMode");
        }

        restart() {
            this.st.restartShardRS(0);
            this.st.restartConfigServer(0);
        }

        stop() {
            this.st.stop();
        }
    }

    return {
        WriteBlockState: WriteBlockState,
        ShardingFixture: ShardingFixture,
        ReplicaFixture: ReplicaFixture,
        bypassUser: bypassUser,
        noBypassUser,
        password: password,
        keyfile: keyfile
    };
})();
