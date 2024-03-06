import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

export const UserWriteBlockHelpers = (function() {
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
                    roles: [
                        // Need for CUD operations
                        {role: "readWriteAnyDatabase", db: "admin"},
                        // Need for DDL operations
                        {role: "dbAdminAnyDatabase", db: "admin"},
                        // Need for importCollection
                        {role: "clusterAdmin", db: "admin"}
                    ]
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
            return fun({
                conn: this.adminConn,
                db: db,
                admin: db.getSiblingDB("admin"),
                coll: coll,
            });
        }

        runInParallelShell(asAdmin, funString) {
            const userName = asAdmin ? bypassUser : noBypassUser;
            return startParallelShell(`{
                    db.getSiblingDB('admin').auth('${userName}', '${password}');
                    (` + funString + `)({conn: db.getMongo()});
            }`, this.conn.port);
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
            return configureFailPoint(targetConn, failpoint);
        }

        setFailPoint(failpointName) {
            throw "UNIMPLEMENTED";
        }

        restart() {
            throw "UNIMPLEMENTED";
        }

        stop() {
            throw "UNIMPLEMENTED";
        }

        stepDown() {
            throw "UNIMPLEMENTED";
        }

        setProfilingLevel(level) {
            throw "UNIMPLEMENTED";
        }

        assertFCV(expectedFCV) {
            throw "UNIMPLEMENTED";
        }
    }

    class ReplicaFixture extends Fixture {
        constructor() {
            const rst = new ReplSetTest({
                nodes: 3,
                nodeOptions: {auth: "", bind_ip_all: ""},
                keyFile: keyfile,
                setParameter: {
                    // Set the history window to zero to explicitly control the oldest timestamp.
                    minSnapshotHistoryWindowInSeconds: 0
                }
            });
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

        setFailPoint(failpointName) {
            return configureFailPoint(this.adminConn, failpointName);
        }

        getAllDbPaths() {
            return this.rst.nodes.map(node => this.rst.getDbPath(node));
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

        stepDown() {
            const primary = this.rst.getPrimary();

            this.rst.asCluster(this.rst.nodes, () => {
                this.rst.awaitReplication();
                assert.commandWorked(primary.adminCommand({replSetStepDown: 20}));
                this.rst.stepUp(primary);
            });
        }

        setProfilingLevel(level) {
            return assert.commandWorked(
                this.adminConn.getDB(jsTestName()).setProfilingLevel(level));
        }

        assertFCV(expectedFCV) {
            const actualFCV = assert
                                  .commandWorked(this.adminConn.getDB('admin').runCommand(
                                      {getParameter: 1, featureCompatibilityVersion: 1}))
                                  .featureCompatibilityVersion.version;
            assert.eq(expectedFCV, actualFCV);
        }
    }

    class ShardingFixture extends Fixture {
        constructor() {
            const st =
                new ShardingTest({shards: 1, rs: {nodes: 3}, auth: "", other: {keyFile: keyfile}});

            super(st.s.port);
            this.st = st;
        }

        getStatus() {
            const backend = this.st.rs0.getPrimary();
            return authutil.asCluster(
                backend, keyfile, () => backend.getDB('admin').serverStatus());
        }

        hangTransition(command, failpoint) {
            const configuredFp = this._hangTransition(this.st.shard0, failpoint);
            const awaitShell =
                startParallelShell(funWithArgs((username, password, command) => {
                                       let admin = db.getSiblingDB("admin");
                                       admin.auth(username, password);
                                       assert.commandWorked(admin.runCommand(command));
                                   }, bypassUser, password, command), this.conn.port);
            configuredFp.wait();

            return {waiter: awaitShell, failpoint: configuredFp};
        }

        restartConfigPrimary() {
            jsTestLog('Restarting config primary');
            this.st.restartConfigServer(this.st.configRS.getPrimary());
        }

        setFailPoint(failpointName) {
            const backend = this.st.rs0.getPrimary();
            return authutil.asCluster(
                backend, keyfile, () => configureFailPoint(backend, failpointName));
        }

        restart() {
            this.st.restartShardRS(0);
            this.st.restartConfigServer(0);
        }

        stop() {
            this.st.stop();
        }

        stepDown() {
            const forceStepDown = function(rst) {
                const primary = rst.getPrimary();

                rst.asCluster(rst.nodes, () => {
                    rst.awaitReplication();
                    assert.commandWorked(primary.adminCommand({replSetStepDown: 20}));
                    rst.stepUp(primary);
                });
            };

            this.st._rs.forEach((rst) => {
                forceStepDown(rst.test);
            });

            forceStepDown(this.st.configRS);
        }

        setProfilingLevel(level) {
            const backend = this.st.rs0.getPrimary();
            return authutil.asCluster(
                backend, keyfile, () => backend.getDB(jsTestName()).setProfilingLevel(level));
        }

        assertFCV(expectedFCV) {
            const configAdminDB = this.st.c0.getDB('admin');
            assert(configAdminDB.auth(bypassUser, password));
            const actualFCV = assert
                                  .commandWorked(configAdminDB.runCommand(
                                      {getParameter: 1, featureCompatibilityVersion: 1}))
                                  .featureCompatibilityVersion.version;
            assert.eq(actualFCV, expectedFCV);
        }

        // TODO (SERVER-83924) Remove once feature flag checks are not necessary.
        checkFailPointEnabled(failpointName) {
            const primary = this.st.rs0.getPrimary();
            return authutil.asCluster(
                primary,
                keyfile,
                () => FeatureFlagUtil.isPresentAndEnabled(primary, failpointName));
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
