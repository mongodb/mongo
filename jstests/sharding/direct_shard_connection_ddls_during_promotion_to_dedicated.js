/**
 * Testing direct shard connection DDLs during and after promotion to sharded cluster with dedicated config server
 * @tags: [
 *   # The test caches authenticated connections, so we do not support stepdowns
 *   does_not_support_stepdowns,
 *   featureFlagPreventDirectShardDDLsDuringPromotion,
 *   # The test creates a sharded cluster with a dedicated config server, so the test is incompatible with fixtures with embedded config servers
 *   config_shard_incompatible,
 * ]
 */

import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

const testCommands = [
    {
        name: "createCollection",
        execute: (db) => db.runCommand({create: "testColl1"}),
    },
    {
        name: "renameCollection",
        execute: (db) =>
            db.getSiblingDB("admin").runCommand({renameCollection: "testDB.testColl1", to: "testDB.testColl1Renamed"}),
    },
    {
        name: "implicitCollectionCreation",
        execute: (db) =>
            db.runCommand({
                insert: "implicitColl0",
                documents: [{z: 1}],
            }),
    },
    // TODO(SERVER-112424): Enable testing of cloneCollectionAsCapped
    //{
    //    name: "cloneCollectionAsCapped",
    //    execute: (db) => db.runCommand(
    //        {cloneCollectionAsCapped: "implicitColl0", toCollection: "implicitColl0Capped", size:
    //        100000}),
    //},
    {
        name: "convertToCapped",
        execute: (db) => db.runCommand({convertToCapped: "testColl1Renamed", size: 10000}),
    },
    {
        name: "collMod",
        execute: (db) => db.runCommand({collMod: "testColl1Renamed", cappedSize: 100000}),
    },
    {
        name: "dropCollection",
        execute: (db) => db.runCommand({drop: "testColl1Renamed"}),
    },
    {
        name: "createIndexes",
        execute: (db) =>
            db.runCommand({
                createIndexes: "testColl0",
                indexes: [{key: {x: 1}, name: "x_1"}],
            }),
    },
    {
        name: "dropIndexes",
        execute: (db) => db.runCommand({dropIndexes: "testColl0", index: "x_1"}),
    },
    {
        name: "dropDatabase",
        execute: (db) => db.runCommand({dropDatabase: 1}),
    },
    {
        name: "applyOps with DDL operation",
        execute: (db) => db.runCommand({applyOps: [{op: "c", ns: "testDB.$cmd", o: {create: "testColl0"}}]}),
    },
    {
        name: "applyOps with CRUD operation",
        execute: (db) => db.runCommand({applyOps: [{op: "i", ns: "testDB.testColl0", o: {_id: 1, x: 1}}]}),
    },
];

describe("Check direct DDLs during promotion and after promotion to sharded cluster with dedicated config server", function () {
    before(function () {
        this.keyFile = "jstests/libs/key1";

        this.createAdminUser = function (conn) {
            const directConnection = conn.getDB("admin");
            directConnection.createUser({user: "admin", pwd: "x", roles: ["__system"]});
            assert(directConnection.auth("admin", "x"), "Authentication failed");
        };

        this.createRegularUser = function (conn, db, user, pwd) {
            assert(conn.getDB("admin").auth("admin", "x"), "Authentication failed");
            conn.getDB(db).createUser({
                user: user,
                pwd: pwd,
                roles: ["dbOwner", {role: "dbAdminAnyDatabase", db: "admin"}],
            });
            const userDirectConnection = new Mongo(conn.host);
            var testDBDirectConnection = userDirectConnection.getDB(db);
            assert(testDBDirectConnection.auth(user, pwd), "Authentication failed");
            return testDBDirectConnection;
        };
    });

    beforeEach(function () {
        jsTest.log.info("Creating sharded cluster st with 0 shards");
        this.cluster = new ShardingTest({name: "st", shards: 0, other: {keyFile: this.keyFile}});

        jsTest.log.info("Creating replica set rs");
        this.rs = new ReplSetTest({name: "rs", nodes: 3, keyFile: this.keyFile});
        this.rs.startSet({shardsvr: ""});
        this.rs.initiate();

        jsTest.log.info("Creating admin user on the cluster");
        this.createAdminUser(this.cluster);

        jsTest.log.info("Creating admin user on replica set");
        this.createAdminUser(this.rs.getPrimary());

        jsTest.log.info("Creating new user with dbOwner permissions on replica set");
        this.testDBDirectConnection = this.createRegularUser(this.rs.getPrimary(), "testDB", "user", "x");

        jsTest.log.info("Inserting data on the replica set");
        assert.commandWorked(this.testDBDirectConnection.testColl0.insertOne({x: 1}));
    });

    afterEach(function () {
        this.cluster.stop();
        this.rs.stopSet();
    });

    it("Direct DDLs started after the initialization of sharding during promotion", () => {
        jsTest.log.info("Setting fail point hangAfterShardingInitialization");
        const configPrimary = this.cluster.configRS.getPrimary();
        const shardInitializationFP = configureFailPoint(configPrimary, "hangAfterShardingInitialization");

        jsTest.log.info("Starting parallel shell for running addShard");
        const addShardParallelShell = startParallelShell(
            funWithArgs(function (url) {
                assert(db.getSiblingDB("admin").auth("admin", "x"), "Authentication failed");
                assert.commandWorked(db.adminCommand({addShard: url}));
                db.getSiblingDB("admin").logout();
            }, this.rs.getURL()),
            this.cluster.s.port,
        );

        jsTest.log.info("Waiting hangAfterShardingInitialization");
        shardInitializationFP.wait();

        const userDirectConnection = new Mongo(this.rs.getPrimary().host);
        this.testDBDirectConnection = userDirectConnection.getDB("testDB");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");
        testCommands.forEach((testCommand) => {
            jsTest.log.info(
                `Checking that ${testCommand.name} is not allowed without directShardOperations permissions`,
            );
            assert.commandFailedWithCode(testCommand.execute(this.testDBDirectConnection), ErrorCodes.Unauthorized);
        });

        this.rs
            .getPrimary()
            .getDB("testDB")
            .grantRolesToUser("user", [{role: "directShardOperations", db: "admin"}]);
        testCommands.forEach((testCommand) => {
            jsTest.log.info(
                `Checking that ${testCommand.name} is allowed with directShardOperations special permissions`,
            );
            assert.commandWorked(testCommand.execute(this.testDBDirectConnection));
        });

        jsTest.log.info("Clearing fail point hangAfterShardingInitialization");
        shardInitializationFP.off();
        addShardParallelShell();
    });

    it("Direct DDLs draining after initialization of sharding during promotion", () => {
        jsTest.log.info("Setting fail point hangDuringDropCollection");
        const dropCmdFP = configureFailPoint(this.rs.getPrimary(), "hangDuringDropCollection");

        jsTest.log.info("Starting parallel shell for running direct DDL operation");
        const dropCmdParallelShell = startParallelShell(() => {
            assert(db.getSiblingDB("testDB").auth("user", "x"), "Authentication failed");
            assert(db.getSiblingDB("testDB").testColl0.drop());
            db.getSiblingDB("testDB").logout();
        }, this.rs.getPrimary().port);

        jsTest.log.info("Waiting hangDuringDropCollection");
        dropCmdFP.wait();

        jsTest.log.info("Setting fail point hangAfterDrainingDDLOperations");
        const configPrimary = this.cluster.configRS.getPrimary();
        const shardAfterDrainingDDLOperationsFP = configureFailPoint(configPrimary, "hangAfterDrainingDDLOperations");

        jsTest.log.info("Starting parallel shell for running addShard");
        const addShardParallelShell = startParallelShell(
            funWithArgs(function (url) {
                assert(db.getSiblingDB("admin").auth("admin", "x"), "Authentication failed");
                assert.commandWorked(db.adminCommand({addShard: url}));
                db.getSiblingDB("admin").logout();
            }, this.rs.getURL()),
            this.cluster.s.port,
        );

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
        addShardParallelShell();
    });

    it("Transactions are aborted during promotion", () => {
        jsTest.log.info("Start a transaction");
        let session = this.testDBDirectConnection.getMongo().startSession();
        session.startTransaction();
        assert.commandWorked(session.getDatabase("testDB").runCommand({create: "foo"}));

        jsTest.log.info("Promote to sharded");
        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.rs.getURL()}));

        jsTest.log.info("Check that the transaction was aborted");
        const res = session.getDatabase("testDB").runCommand({insert: "foo", documents: [{x: 1}]});
        assert.commandFailedWithCode(res, ErrorCodes.NoSuchTransaction);
        assert(TxnUtil.isTransientTransactionError(res));
    });

    it("Direct DDLs after promotion", () => {
        jsTest.log.info("Adding replica set as a shard to the cluster");
        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.rs.getURL()}));

        const userDirectConnection = new Mongo(this.rs.getPrimary().host);
        this.testDBDirectConnection = userDirectConnection.getDB("testDB");
        assert(this.testDBDirectConnection.auth("user", "x"), "Authentication failed");
        testCommands.forEach((testCommand) => {
            jsTest.log.info(
                `Checking that ${testCommand.name} is not allowed without directShardOperations permissions`,
            );
            assert.commandFailedWithCode(testCommand.execute(this.testDBDirectConnection), ErrorCodes.Unauthorized);
        });

        this.rs
            .getPrimary()
            .getDB("testDB")
            .grantRolesToUser("user", [{role: "directShardOperations", db: "admin"}]);
        testCommands.forEach((testCommand) => {
            jsTest.log.info(`Checking that ${testCommand.name} is allowed with directShardOperations special
                    permissions`);
            assert.commandWorked(testCommand.execute(this.testDBDirectConnection));
        });

        jsTest.log.info("Checking that DDLs are allowed through mongos");
        // We test all the test commands, except applyOps since it cannot be run through mongos
        testCommands.slice(0, -2).forEach((testCommand) => {
            jsTest.log.info(`Checking that ${testCommand.name} is allowed through mongos`);
            assert.commandWorked(testCommand.execute(this.cluster.s.getDB("testDB")));
        });
    });
});
