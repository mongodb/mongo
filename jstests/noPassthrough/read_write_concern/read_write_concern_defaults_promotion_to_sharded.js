/* Tests default read/write concern value consistency before, during, and after promotion to sharded.
 * @tags: [
 *   requires_persistence,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {describe, beforeEach, afterEach, it} from "jstests/libs/mochalite.js";
import {stopReplicationOnSecondaries, restartReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("Read/write concern defaults directly against shard servers", function () {
    beforeEach(() => {
        let verbosity = {replication: 2, command: 2};
        this.replSet = new ReplSetTest({
            nodes: 2,
            nodeOptions: {
                setParameter: {logComponentVerbosity: verbosity},
            },
        });
        this.replSet.startSet();
        this.replSet.initiate();
        this.cluster = undefined;
        this.mongos = undefined;

        this.dbName = "test";
        this.collName = "foo";
        this.counter = 1;
        assert.commandWorked(this.replSet.getPrimary().getDB(this.dbName).createCollection(this.collName));

        // This function expects the implicit defaults to be:
        //   defaultWriteConcern: {w: "majority", wtimeout: 0}
        //   defaultReadConcern: {level: "local"}
        this.checkImplicitDefaults = function () {
            jsTest.log.info("Stop replication on secondaries");
            stopReplicationOnSecondaries(this.replSet, false);

            jsTest.log.info("Do a write, it should time out due to missing majority");
            assert.commandFailedWithCode(
                this.replSet
                    .getPrimary()
                    .getDB(this.dbName)
                    .runCommand({insert: this.collName, documents: [{x: this.counter}], maxTimeMS: 500}),
                ErrorCodes.MaxTimeMSExpired,
            );

            jsTest.log.info("Do a read, it should return the document anyways since the default is local");
            let docFound = this.replSet
                .getPrimary()
                .getDB(this.dbName)
                .getCollection(this.collName)
                .count({x: this.counter});
            assert.eq(1, docFound);

            this.counter++;

            jsTest.log.info("Restart replication and wait for steady");
            restartReplicationOnSecondaries(this.replSet);
            this.replSet.awaitReplication();
        };

        // This function expects the user defaults to be:
        //   defaultWriteConcern: {w: "majority", wtimeout: 500}
        //   defaultReadConcern: {level: "majority"}
        // These values were chosen both for ease of testing and to ensure we aren't overlapping
        // with the implicit or empty constructor defaults.
        this.checkUserSpecifiedDefaults = function () {
            jsTest.log.info("Stop replication on secondaries");
            stopReplicationOnSecondaries(this.replSet, false);

            jsTest.log.info("Do a write, it should time out due to missing majority");
            assert.commandFailedWithCode(
                this.replSet
                    .getPrimary()
                    .getDB(this.dbName)
                    .runCommand({insert: this.collName, documents: [{x: this.counter}]}),
                ErrorCodes.WriteConcernTimeout,
            );

            jsTest.log.info("Do a read, it should not return the document since the read concern is majority");
            let docFound = this.replSet
                .getPrimary()
                .getDB(this.dbName)
                .getCollection(this.collName)
                .count({x: this.counter});
            assert.eq(0, docFound);

            this.counter++;

            jsTest.log.info("Restart replication and wait for steady");
            restartReplicationOnSecondaries(this.replSet);
            this.replSet.awaitReplication();
        };
    });

    afterEach(() => {
        if (this.cluster !== undefined) {
            this.cluster.stop();
        }
        if (this.mongos !== undefined) {
            MongoRunner.stopMongos(this.mongos);
        }
        this.replSet.stopSet();
    });

    it("Implicit default", () => {
        jsTest.log.info("Check implicit defaults as a normal replica set");
        this.checkImplicitDefaults();

        jsTest.log.info("Check implicit defaults when started with --shardsvr");
        this.replSet.stopSet(null, true, {});
        this.replSet.startSet({"shardsvr": ""}, true);
        this.checkImplicitDefaults();

        jsTest.log.info("Check implicit defaults after being added to the cluster");
        this.cluster = new ShardingTest({shards: 0});
        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.replSet.getURL()}));
        // Fetch the sharding metadata so that the write doesn't have to do a refresh.
        assert.commandWorked(
            this.replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: this.dbName + "." + this.collName}),
        );
        this.checkImplicitDefaults();
    });

    it("Implicit default for config server promotion", () => {
        jsTest.log.info("Check implicit defaults as a normal replica set");
        this.checkImplicitDefaults();

        jsTest.log.info("Check implicit defaults when started with --shardsvr");
        this.replSet.stopSet(null, true, {});
        this.replSet.startSet({"configsvr": "", replicaSetConfigShardMaintenanceMode: ""}, true);
        this.checkImplicitDefaults();

        jsTest.log.info("Check implicit defaults after being added to the cluster");
        this.mongos = MongoRunner.runMongos({configdb: this.replSet.getURL()});
        assert.commandWorked(this.mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));
        // Fetch the sharding metadata so that the write doesn't have to do a refresh.
        assert.commandWorked(
            this.replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: this.dbName + "." + this.collName}),
        );
        this.checkImplicitDefaults();
    });

    it("User specified default", () => {
        jsTest.log.info("Set the defaults to something other than the implicit default");
        this.cluster = new ShardingTest({shards: 0});
        let conns = [this.replSet.getPrimary(), this.cluster.s];
        conns.forEach((conn) => {
            assert.commandWorked(
                conn.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: "majority", wtimeout: 500},
                    defaultReadConcern: {level: "majority"},
                    writeConcern: {w: "majority"},
                }),
            );
        });

        jsTest.log.info("Check user specified defaults as a normal replica set");
        this.checkUserSpecifiedDefaults();

        jsTest.log.info("Check user specified defaults when started with --shardsvr");
        this.replSet.stopSet(null, true, {});
        this.replSet.startSet({"shardsvr": ""}, true);
        this.replSet.awaitReplication();
        this.checkUserSpecifiedDefaults();

        jsTest.log.info("Check user specified defaults after being added to the cluster");
        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.replSet.getURL()}));
        // Fetch the sharding metadata so that the write doesn't have to do a refresh.
        assert.commandWorked(
            this.replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: this.dbName + "." + this.collName}),
        );
        this.checkUserSpecifiedDefaults();
    });

    it("User specified default for config server promotion", () => {
        jsTest.log.info("Set the defaults to something other than the implicit default");
        assert.commandWorked(
            this.replSet.getPrimary().adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: "majority", wtimeout: 500},
                defaultReadConcern: {level: "majority"},
                writeConcern: {w: "majority"},
            }),
        );

        jsTest.log.info("Check user specified defaults as a normal replica set");
        this.checkUserSpecifiedDefaults();

        jsTest.log.info("Check user specified defaults when started with --configsvr");
        this.replSet.stopSet(null, true, {});
        this.replSet.startSet({"configsvr": "", replicaSetConfigShardMaintenanceMode: ""}, true);
        this.replSet.awaitReplication();
        this.checkUserSpecifiedDefaults();

        jsTest.log.info("Check user specified defaults after being added to the cluster");
        this.mongos = MongoRunner.runMongos({configdb: this.replSet.getURL()});
        assert.commandWorked(this.mongos.adminCommand({transitionFromDedicatedConfigServer: 1}));
        // Fetch the sharding metadata so that the write doesn't have to do a refresh.
        assert.commandWorked(
            this.replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: this.dbName + "." + this.collName}),
        );
        this.checkUserSpecifiedDefaults();
    });

    it("Changing the default on the cluster does not change the shard's default", () => {
        jsTest.log.info("Add the shard to the cluster");
        this.cluster = new ShardingTest({shards: 0});
        this.replSet.stopSet(null, true, {});
        this.replSet.startSet({"shardsvr": ""}, true);
        this.replSet.awaitReplication();
        assert.commandWorked(this.cluster.s.adminCommand({addShard: this.replSet.getURL()}));
        this.replSet.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: this.dbName + "." + this.collName});

        jsTest.log.info("Change the defaults on the config server");
        assert.commandWorked(
            this.cluster.s.adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1, wtimeout: 0},
                defaultReadConcern: {level: "majority"},
                writeConcern: {w: "majority"},
            }),
        );

        jsTest.log.info("Check that the defaults are still the implicit ones via direct connection");
        this.checkImplicitDefaults();

        jsTest.log.info("Verify that the defaults cannot be modified on the shard");
        assert.commandFailedWithCode(
            this.replSet.getPrimary().adminCommand({
                setDefaultRWConcern: 1,
                defaultWriteConcern: {w: 1, wtimeout: 0},
                defaultReadConcern: {level: "majority"},
                writeConcern: {w: "majority"},
            }),
            51301,
        );
    });
});
