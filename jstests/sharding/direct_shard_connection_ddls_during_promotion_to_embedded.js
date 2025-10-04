/**
 * Testing direct shard connection DDLs during and after promotion to sharded cluster with embedded config server
 * @tags: [
 *   # The test caches authenticated connections, so we do not support stepdowns
 *   does_not_support_stepdowns,
 *   featureFlagPreventDirectShardDDLsDuringPromotion,
 *   requires_persistence
 * ]
 */

import {before, beforeEach, afterEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

describe("Check direct DDLs during promotion and after promotion to sharded cluster with embedded config server", function () {
    before(function () {
        this.keyFile = "jstests/libs/key1";

        this.doRollingRestart = (rs, startupFlags) => {
            rs.awaitReplication();
            for (const node of rs.getSecondaries()) {
                const id = rs.getNodeId(node);
                rs.stop(
                    id,
                    null,
                    {},
                    {
                        forRestart: true,
                        waitPid: true,
                    },
                );
                assert.doesNotThrow(() => {
                    rs.start(id, {
                        ...startupFlags,
                        remember: false,
                    });
                });
            }

            const primaryId = rs.getNodeId(rs.getPrimary());
            const secondary = rs.getSecondary();

            // We wait for the stepup writes explicitly as the stepup calls the waitForStepUpWrites
            // on the primary, but we call the asCluster on the secondary (so it will hang there,
            // because of lack of authorization).
            rs.asCluster(
                rs.getPrimary(),
                () => {
                    rs.waitForStepUpWrites();
                },
                this.keyFile,
            );

            rs.asCluster(
                secondary,
                () => {
                    rs.stepUp(secondary, {
                        awaitReplicationBeforeStepUp: true,
                        awaitWritablePrimary: true,
                        doNotWaitForPrimaryOnlyServices: true,
                    });
                },
                this.keyFile,
            );

            rs.stop(
                primaryId,
                null,
                {},
                {
                    forRestart: true,
                    waitPid: true,
                },
            );
            assert.doesNotThrow(() => {
                rs.start(primaryId, {
                    ...startupFlags,
                    remember: false,
                });
            });
        };

        this.createAdminUser = function (conn) {
            const directConnection = conn.getDB("admin");
            directConnection.createUser({user: "admin", pwd: "x", roles: ["__system"]});
            assert(directConnection.auth("admin", "x"), "Authentication failed");
        };

        this.createRegularUser = function (conn, db, user, pwd) {
            assert(conn.getDB("admin").auth("admin", "x"), "Authentication failed");
            conn.getDB(db).createUser({user: user, pwd: pwd, roles: ["dbOwner"]});
            const userDirectConnection = new Mongo(conn.host);
            var testDBDirectConnection = userDirectConnection.getDB(db);
            assert(testDBDirectConnection.auth(user, pwd), "Authentication failed");
            return testDBDirectConnection;
        };
    });

    beforeEach(function () {
        jsTest.log.info("Creating config server replica set");
        this.configRS = new ReplSetTest({nodes: 3, keyFile: this.keyFile});

        jsTest.log.info("Starting config server replica set");
        this.configRS.startSet({remember: false});

        jsTest.log.info("Initiating config server replica set");
        this.configRS.asCluster(
            this.configRS.nodes[0],
            () => {
                this.configRS.initiate();
            },
            this.keyFile,
        );

        jsTest.log.info("Creating admin user on the config server replica set");
        this.createAdminUser(this.configRS.getPrimary());

        jsTest.log.info("Creating new user with dbOwner permissions on config server replica set");
        this.testDBDirectConnection = this.createRegularUser(this.configRS.getPrimary(), "testDB", "user", "x");

        jsTest.log.info("Inserting data on the config server replica set");
        assert.commandWorked(this.testDBDirectConnection.testColl.insertOne({x: 1}));

        jsTest.log.info("Restarting config server replica set nodes with configsvr and maintenance mode options");
        this.doRollingRestart(this.configRS, {
            configsvr: "",
            replicaSetConfigShardMaintenanceMode: "",
        });

        jsTest.log.info("Get and update the config server replica set config");
        let config = {};
        this.configRS.asCluster(
            this.configRS.getPrimary(),
            () => {
                config = this.configRS.getReplSetConfigFromNode();
            },
            this.keyFile,
        );
        config.configsvr = true;
        config.version = config.version + 1;
        this.configRS.asCluster(
            this.configRS.getPrimary(),
            () => {
                assert.commandWorked(this.configRS.getPrimary().adminCommand({replSetReconfig: config}));
            },
            this.keyFile,
        );

        jsTest.log.info("Restarting config server replica set nodes to exit maintenance mode");
        this.doRollingRestart(this.configRS, {
            configsvr: "",
        });

        jsTest.log.info("Checking that sharding is not yet initialized on the config server replica set");
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            const res = assert.commandWorked(this.configRS.getPrimary().getDB("admin").runCommand({shardingState: 1}));
            assert.eq(res.enabled, false);
        });

        jsTest.log.info("Starting mongos node binded to config server replica set");
        this.mongos = MongoRunner.runMongos({keyFile: this.keyFile, configdb: this.configRS.getURL()});
    });

    afterEach(function () {
        MongoRunner.stopMongos(this.mongos);
        this.configRS.stopSet();
    });

    it("Direct DDLs started after the initialization of sharding during promotion", () => {
        jsTest.log.info("Setting fail point hangAfterShardingInitialization");
        const shardInitializationFP = configureFailPoint(this.configRS.getPrimary(), "hangAfterShardingInitialization");

        jsTest.log.info(
            "Starting parallel shell for promoting replica set to sharded cluster with embedded config server",
        );
        const promotionParallelShell = startParallelShell(() => {
            assert(db.getSiblingDB("admin").auth("admin", "x"), "Authentication failed");
            assert(db.getSiblingDB("admin").runCommand({"transitionFromDedicatedConfigServer": 1}));
            db.getSiblingDB("admin").logout();
        }, this.mongos.port);

        jsTest.log.info("Waiting hangAfterShardingInitialization");
        shardInitializationFP.wait();

        jsTest.log.info("Checking that direct DDLs are disallowed without special permissions");
        const userDirectConnection = new Mongo(this.configRS.getPrimary().host);
        this.testDBDirectConnection = userDirectConnection.getDB("testDB");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");
        assert.commandFailedWithCode(this.testDBDirectConnection.dropDatabase(), ErrorCodes.Unauthorized);

        jsTest.log.info("Checking that direct DDLs are allowed with special permissions");
        this.configRS.asCluster(
            this.configRS.getPrimary(),
            () => {
                this.configRS
                    .getPrimary()
                    .getDB("testDB")
                    .grantRolesToUser("user", [{role: "directShardOperations", db: "admin"}]);
            },
            this.keyFile,
        );
        jsTest.log.info("Running dropDatabase command with special permissions");
        assert.commandWorked(this.testDBDirectConnection.dropDatabase());

        jsTest.log.info("Clearing fail point hangAfterShardingInitialization");
        shardInitializationFP.off();
        promotionParallelShell();

        jsTest.log.info("Checking that sharding is initialized on the config server replica set");
        this.configRS.asCluster(this.configRS.getPrimary(), () => {
            const res = assert.commandWorked(this.configRS.getPrimary().getDB("admin").runCommand({shardingState: 1}));
            assert.eq(res.enabled, true);
        });
    });

    it("Direct DDLs draining after initialization of sharding during promotion", () => {
        jsTest.log.info("Setting fail point hangDuringDropCollection");
        const dropCmdFP = configureFailPoint(this.configRS.getPrimary(), "hangDuringDropCollection");

        jsTest.log.info("Starting parallel shell for running direct DDL operation");
        const dropCmdParallelShell = startParallelShell(() => {
            assert(db.getSiblingDB("testDB").auth("user", "x"), "Authentication failed");
            assert(db.getSiblingDB("testDB").testColl.drop());
            db.getSiblingDB("testDB").logout();
        }, this.configRS.getPrimary().port);

        jsTest.log.info("Waiting hangDuringDropCollection");
        dropCmdFP.wait();

        jsTest.log.info("Setting fail point hangAfterDrainingDDLOperations");
        const configPrimary = this.configRS.getPrimary();
        const shardAfterDrainingDDLOperationsFP = configureFailPoint(configPrimary, "hangAfterDrainingDDLOperations");

        jsTest.log.info(
            "Starting parallel shell for promoting replica set to sharded cluster with embedded config server",
        );
        const promotionParallelShell = startParallelShell(() => {
            assert(db.getSiblingDB("admin").auth("admin", "x"), "Authentication failed");
            assert(db.getSiblingDB("admin").runCommand({"transitionFromDedicatedConfigServer": 1}));
            db.getSiblingDB("admin").logout();
        }, this.mongos.port);

        jsTest.log.info("Checking that the draining of DDls operations is timing out (timeout set to 15s)");
        const TIMEOUT_MS = 15000;
        assert(
            !shardAfterDrainingDDLOperationsFP.waitWithTimeout(TIMEOUT_MS),
            "hangAfterDrainingDDLOperations fail point wait did not timed out as expected",
        );

        jsTest.log.info("Clearing hangDuringDropCollection fail point");
        dropCmdFP.off();
        dropCmdParallelShell();

        jsTest.log.info("Clearing hangAfterDrainingDDLOperations fail point");
        shardAfterDrainingDDLOperationsFP.off();
        promotionParallelShell();
    });

    it("Direct DDLs after promotion", () => {
        jsTest.log.info("Promoting replica set to sharded cluster with embedded config server");
        const adminDBMongosConnection = this.mongos.getDB("admin");
        assert(adminDBMongosConnection.auth("admin", "x"), "Authentication failed");
        assert.commandWorked(adminDBMongosConnection.adminCommand({"transitionFromDedicatedConfigServer": 1}));

        jsTest.log.info("Checking that direct DDLs are disallowed without special permissions");
        const userDirectConnection = new Mongo(this.configRS.getPrimary().host);
        this.testDBDirectConnection = userDirectConnection.getDB("testDB");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");
        assert.commandFailedWithCode(this.testDBDirectConnection.dropDatabase(), ErrorCodes.Unauthorized);

        jsTest.log.info("Checking that direct DDLs are allowed with special permissions");
        this.configRS.asCluster(
            this.configRS.getPrimary(),
            () => {
                this.configRS
                    .getPrimary()
                    .getDB("testDB")
                    .grantRolesToUser("user", [{role: "directShardOperations", db: "admin"}]);
            },
            this.keyFile,
        );
        assert.commandWorked(this.testDBDirectConnection.dropDatabase());

        jsTest.log.info("Checking that DDLs are allowed through mongos");
        assert.commandWorked(this.mongos.getDB("testDB").testColl.insertOne({x: 1}));
    });
});
