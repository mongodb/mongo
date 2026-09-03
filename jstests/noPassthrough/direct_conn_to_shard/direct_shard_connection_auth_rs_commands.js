/**
 * Tests that direct shard connections are correctly allowed and disallowed using authentication.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function awaitReplication(rst) {
    authutil.asCluster(rst.nodes, "jstests/libs/key1", function () {
        rst.awaitReplication();
    });
}

function setupConn(conn, user, pwd) {
    const newConn = new Mongo(conn.host);
    assert(newConn.getDB("admin").auth(user, pwd), "Authentication failed");
    return newConn;
}

function runAsAdminUser(conn, cmd, database) {
    const newConn = setupConn(conn, "admin", "x");
    const res = assert.commandWorked(newConn.getDB(database).runCommand(cmd));
    newConn.close();
    return res;
}

describe("direct shard connections with replica-set administration commands", function () {
    before(function () {
        this.st = new ShardingTest({
            name: jsTestName(),
            keyFile: "jstests/libs/key1",
            shards: 2,
            // Disallow chaining so that when testing replSetAbortPrimaryCatchUp we have the
            // guarantee that the secondary is syncing from the primary.
            rs0: {nodes: 3, settings: {chainingAllowed: false}},
            // By default, our test infrastructure sets the election timeout to a very high value (24
            // hours). For this test, we need a shorter election timeout because it relies on nodes running
            // an election when they do not detect an active primary. Therefore, we are setting the
            // electionTimeoutMillis to its default value.
            initiateWithDefaultElectionTimeout: true,
        });
        this.testDbName = jsTestName();

        const shardAdminDB = this.st.rs0.getPrimary().getDB("admin");
        jsTest.log.info("Setup users for test");
        assert.commandWorked(
            shardAdminDB.runCommand({createUser: "admin", pwd: "x", roles: ["root"]}),
        );
        assert(shardAdminDB.auth("admin", "x"), "Authentication failed");
        assert.commandWorked(
            shardAdminDB.runCommand({
                setParameter: 1,
                logComponentVerbosity: {sharding: {verbosity: 2}, assert: {verbosity: 1}},
            }),
        );
        // The replSetStateChange action type is needed for this test.
        assert.commandWorked(
            shardAdminDB.runCommand({
                createUser: "user",
                pwd: "y",
                roles: ["clusterManager"],
            }),
        );
        shardAdminDB.logout();

        jsTest.log.info("Await replication to ensure the user is created on all nodes");
        awaitReplication(this.st.rs0);
    });

    afterEach(function () {
        // awaitSecondaryNodes() does not ensure that all non-primary nodes have wrapped up
        // outstanding elections. ReplSetFreeze is used here to ensure that the secondaries are
        // fully secondary before starting a new test case.
        authutil.asCluster(this.st.rs0.nodes, "jstests/libs/key1", () => {
            assert.soonNoExcept(() => {
                const primary = this.st.rs0.getPrimary();

                for (const node of this.st.rs0.nodes) {
                    if (node === primary) {
                        continue;
                    }

                    const result = node.adminCommand({replSetFreeze: 0});
                    if (!result.ok) {
                        return false;
                    }
                }
                return true;
            }, "waiting for all non-primary members to leave election");
        });
    });

    after(function () {
        const primaryConn = setupConn(this.st.rs0.getPrimary(), "admin", "x");
        const primaryAdminDB = primaryConn.getDB("admin");
        assert.commandWorked(primaryAdminDB.runCommand({dropUser: "user"}));
        primaryConn.close();

        this.st.stop();
    });

    it("allows replSetStepDown and replSetStepUp", function () {
        const secondaryConn = setupConn(this.st.rs0.getSecondary(), "user", "y");
        assert.commandWorked(secondaryConn.adminCommand({replSetStepUp: 1}));
        authutil.asCluster(this.st.rs0.nodes, "jstests/libs/key1", () => {
            this.st.rs0.awaitNodesAgreeOnPrimary();
            this.st.rs0.awaitSecondaryNodes();
        });

        assert.soonNoExcept(() => {
            // {replSetStepDown: 0} defaults to 60 seconds rather than 0, so specify 1 here and unfreeze
            // the node later in the test if it will be stepped up to primary.
            return secondaryConn.adminCommand({replSetStepDown: 1, force: true}).ok;
        }, `failed attempt to step down node ${secondaryConn.host}`);
        authutil.asCluster(this.st.rs0.nodes, "jstests/libs/key1", () => {
            this.st.rs0.awaitNodesAgreeOnPrimary();
            this.st.rs0.awaitSecondaryNodes();
        });
        secondaryConn.close();
    });

    it("allows replSetFreeze", function () {
        const secondaryConn = setupConn(this.st.rs0.getSecondary(), "user", "y");
        assert.commandWorked(secondaryConn.adminCommand({replSetFreeze: 1}));
        assert.commandWorked(secondaryConn.adminCommand({replSetFreeze: 0}));
        secondaryConn.close();
    });

    it("allows replSetMaintenance", function () {
        const secondaryConn = setupConn(this.st.rs0.getSecondary(), "user", "y");
        assert.commandWorked(secondaryConn.adminCommand({replSetMaintenance: 1}));
        assert.commandWorked(secondaryConn.adminCommand({replSetMaintenance: 0}));
        secondaryConn.close();
    });

    it("allows replSetSyncFrom", function () {
        const primary = this.st.rs0.getPrimary();
        const secondary1 = this.st.rs0.getSecondaries()[0];
        const secondary2Conn = setupConn(this.st.rs0.getSecondaries()[1], "user", "y");

        assert.commandWorked(secondary2Conn.adminCommand({replSetSyncFrom: secondary1.name}));
        assert.commandWorked(secondary2Conn.adminCommand({replSetSyncFrom: primary.name}));
        secondary2Conn.close();
    });

    it("allows replSetAbortPrimaryCatchUp", function () {
        const primary = this.st.rs0.getPrimary();
        const secondary1 = this.st.rs0.getSecondaries()[0];
        const secondary2 = this.st.rs0.getSecondaries()[1];

        const primaryConn = setupConn(primary, "user", "y");
        const secondary1Conn = setupConn(secondary1, "user", "y");
        const secondary2Conn = setupConn(secondary2, "user", "y");
        // Ensure the secondary we plan to step up isn't frozen from a prior step down
        assert.commandWorked(secondary1Conn.adminCommand({replSetFreeze: 0}));

        // Reconfig to make the catchup timeout infinite.
        const newConfig = assert.commandWorked(
            primaryConn.adminCommand({replSetGetConfig: 1}),
        ).config;
        newConfig.version++;
        newConfig.settings.catchUpTimeoutMillis = -1;
        assert.commandWorked(primaryConn.adminCommand({replSetReconfig: newConfig}));
        // Put new secondary into primary catch up
        const stopReplProducerFailPoint2 = configureFailPoint(secondary2Conn, "stopReplProducer");
        stopReplProducerFailPoint2.wait();
        runAsAdminUser(primary, {insert: "catch_up", documents: [{_id: 0}]}, this.testDbName);
        assert.soon(() => {
            const count = runAsAdminUser(
                secondary1,
                {
                    count: "catch_up",
                    readConcern: {level: "local"},
                    $readPreference: {mode: "secondary"},
                },
                this.testDbName,
            ).n;
            return count === 1;
        });

        const stopReplProducerFailPoint1 = configureFailPoint(secondary1Conn, "stopReplProducer");
        stopReplProducerFailPoint1.wait();
        // Since we have stopped replication on both secondaries, run this with w: 1
        runAsAdminUser(
            primary,
            {insert: "catch_up", documents: [{_id: 1}], writeConcern: {w: 1}},
            this.testDbName,
        );
        // Step up the first secondary, it will enter catch up indefinitely
        assert.commandWorked(secondary1Conn.adminCommand({replSetStepUp: 1}));

        // Now we can abort the catch up
        assert.commandWorked(secondary1Conn.adminCommand({replSetAbortPrimaryCatchUp: 1}));

        stopReplProducerFailPoint1.off();
        stopReplProducerFailPoint2.off();

        primaryConn.close();
        secondary1Conn.close();
        secondary2Conn.close();

        authutil.asCluster(this.st.rs0.nodes, "jstests/libs/key1", () => {
            this.st.rs0.awaitNodesAgreeOnPrimary();
            this.st.rs0.awaitSecondaryNodes();
        });
    });
});
