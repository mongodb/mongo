//
// MultiVersion utility functions for clusters
//

import "jstests/multiVersion/libs/multi_rs.js";

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

/**
 * Restarts the specified binaries in options with the specified binVersion. The binaries are
 * started with the --upgradeBackCompat CLI option. This function can be called several times with
 * different combinations of `upgradeShards`, `upgradeConfigs` and `upgradeMongos` to upgrade
 * selectively different parts of the cluster. After all binaries have been upgraded, you need to
 * call @see ShardingTest.restartBinariesWithoutUpgradeBackCompat to complete the cluster upgrade.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     upgradeShards: <bool>, // defaults to true
 *     upgradeOneShard: <rs> // defaults to false
 *     upgradeConfigs: <bool>, // defaults to true
 *     upgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world upgrade behavior, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
ShardingTest.prototype.upgradeBinariesWithBackCompat = function(binVersion, options) {
    this._restartBinariesForUpgrade(binVersion, options, true);
};

/**
 * Restarts the specified binaries in options with the specified binVersion. The binaries are
 * started without the --upgradeBackCompat CLI option. This function can be called several times
 * with different combinations of `upgradeShards`, `upgradeConfigs` and `upgradeMongos` to restart
 * selectively different parts of the cluster. To perform a correct cluster upgrade, this
 * function should be called after @see ShardingTest.upgradeBinariesWithBackCompat , with the same
 * `binVersion`, and after all binaries have been started with --upgradeBackCompat.
 *
 * Note: this does not perform any upgrade operations.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     upgradeShards: <bool>, // defaults to true
 *     upgradeOneShard: <rs> // defaults to false
 *     upgradeConfigs: <bool>, // defaults to true
 *     upgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world upgrade behavior, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
ShardingTest.prototype.restartBinariesWithoutUpgradeBackCompat = function(binVersion, options) {
    this._restartBinariesForUpgrade(binVersion, options, false);
};

/**
 * Restarts the cluster with the specified binVersion. The complete binary update procedure is
 * carried out, but no upgrade operations are issued. If you want more fine control over the binary
 * update procedure, use @see ShardingTest.upgradeBinariesWithBackCompat and
 * @see ShardingTest.restartBinariesWithoutUpgradeBackCompat .
 *
 * @param binVersion {string}
 * @param waitUntilStable {bool} defaults to false since it provides a more realistic
 *                               approximation of real-world upgrade behavior, even though
 *                               certain tests will likely want a stable cluster after upgrading.
 */
ShardingTest.prototype.upgradeCluster = function(binVersion, waitUntilStable = false) {
    if (typeof waitUntilStable !== "boolean") {
        throw new Error(
            "Use upgradeBinariesWithBackCompat and restartBinariesWithoutUpgradeBackCompat for " +
            "fine control over the binary upgrade process");
    }
    this.upgradeBinariesWithBackCompat(binVersion, {waitUntilStable: waitUntilStable});
    this.restartBinariesWithoutUpgradeBackCompat(binVersion, {waitUntilStable: waitUntilStable});
};

ShardingTest.prototype._restartBinariesForUpgrade = function(
    binVersion, options, upgradeBackCompat) {
    options = options || {};
    if (options.upgradeShards == undefined)
        options.upgradeShards = true;
    if (options.upgradeOneShard == undefined)
        options.upgradeOneShard = false;
    if (options.upgradeConfigs == undefined)
        options.upgradeConfigs = true;
    if (options.upgradeMongos == undefined)
        options.upgradeMongos = true;
    if (options.waitUntilStable == undefined)
        options.waitUntilStable = false;

    var upgradeOptions = {binVersion: binVersion};
    if (upgradeBackCompat) {
        upgradeOptions.upgradeBackCompat = "";
        upgradeOptions.removeOptions = ["downgradeBackCompat"];
    } else {
        upgradeOptions.removeOptions = ["upgradeBackCompat", "downgradeBackCompat"];
    }

    if (options.upgradeConfigs) {
        this.configRS.upgradeSet(upgradeOptions);
        for (var i = 0; i < this.configRS.nodes.length; i++) {
            this["config" + i] = this.configRS.nodes[i];
            this["c" + i] = this.configRS.nodes[i];
        }
    }

    if (options.upgradeShards) {
        // Upgrade shards
        this._rs.forEach((rs) => {
            rs.test.upgradeSet(upgradeOptions);
        });
    }

    if (options.upgradeOneShard) {
        // Upgrade one shard.
        let rs = options.upgradeOneShard;
        rs.upgradeSet(upgradeOptions);
    }

    if (options.upgradeMongos) {
        // Upgrade all mongos hosts if specified
        var numMongoses = this._mongos.length;

        for (var i = 0; i < numMongoses; i++) {
            var mongos = this._mongos[i];
            MongoRunner.stopMongos(mongos);

            mongos = MongoRunner.runMongos({...upgradeOptions, restart: mongos});

            this["s" + i] = this._mongos[i] = mongos;
            if (i == 0)
                this.s = mongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }

    if (options.waitUntilStable) {
        this.waitUntilStable();
    }
};

/**
 * Restarts the specified binaries in options. `binVersion` must be the currently running
 * binary version (due to some limitations, the function cannot automatically determine which bin
 * version is currently running). The binaries are started with the --downgradeBackCompat CLI
 * option. This function can be called several times with different combinations of
 * `downgradeShards`, `downgradeConfigs` and `downgradeMongos` to restart selectively different
 * parts of the cluster. After all binaries have been restarted, you need to call
 * @see ShardingTest.downgradeBinariesWithoutDowngradeBackCompat to complete the cluster downgrade.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     downgradeShards: <bool>, // defaults to true
 *     downgradeOneShard: <rs> // defaults to false
 *     downgradeConfigs: <bool>, // defaults to true
 *     downgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world downgrade behavior, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
ShardingTest.prototype.restartBinariesWithDowngradeBackCompat = function(binVersion, options) {
    this._restartBinariesForDowngrade(binVersion, options, true);
};

/**
 * Restarts the specified binaries in options with the specified binVersion. The binaries are
 * started without the --downgradeBackCompat CLI option. This function can be called several times
 * with different combinations of `downgradeShards`, `downgradeConfigs` and `downgradeMongos` to
 * downgrade selectively different parts of the cluster. To perform a correct cluster downgrade,
 * this function should be called after @see ShardingTest.restartBinariesWithDowngradeBackCompat and
 * after all binaries have been started with --downgradeBackCompat.
 *
 * Note: this does not perform any downgrade operations.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     downgradeShards: <bool>, // defaults to true
 *     downgradeOneShard: <rs> // defaults to false
 *     downgradeConfigs: <bool>, // defaults to true
 *     downgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world downgrade behavior, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
ShardingTest.prototype.downgradeBinariesWithoutDowngradeBackCompat = function(binVersion, options) {
    this._restartBinariesForDowngrade(binVersion, options, false);
};

/**
 * Restarts the cluster with the specified binVersion. The complete binary downgrade procedure is
 * carried out, but no downgrade operations are issued. If you want more fine control over the
 * binary downgrade procedure, use @see ShardingTest.restartBinariesWithDowngradeBackCompat and
 * @see ShardingTest.downgradeBinariesWithoutDowngradeBackCompat .
 *
 * @param binVersion {string}
 * @param waitUntilStable {bool} defaults to false since it provides a more realistic
 *                               approximation of real-world downgrade behavior, even though
 *                               certain tests will likely want a stable cluster after upgrading.
 */
ShardingTest.prototype.downgradeCluster = function(
    fromBinVersion, toVersion, waitUntilStable = false) {
    if (typeof waitUntilStable !== "boolean") {
        throw new Error(
            "Use restartBinariesWithDowngradeBackCompat and " +
            "downgradeBinariesWithoutDowngradeBackCompat for fine control over the binary " +
            "downgrade process");
    }
    this.restartBinariesWithDowngradeBackCompat(fromBinVersion, {waitUntilStable: waitUntilStable});
    this.downgradeBinariesWithoutDowngradeBackCompat(toVersion, {waitUntilStable: waitUntilStable});
};

ShardingTest.prototype._restartBinariesForDowngrade = function(
    binVersion, options, downgradeBackCompat) {
    options = options || {};
    if (options.downgradeShards == undefined)
        options.downgradeShards = true;
    if (options.downgradeOneShard == undefined)
        options.downgradeOneShard = false;
    if (options.downgradeConfigs == undefined)
        options.downgradeConfigs = true;
    if (options.downgradeMongos == undefined)
        options.downgradeMongos = true;
    if (options.waitUntilStable == undefined)
        options.waitUntilStable = false;

    var downgradeOptions = {binVersion: binVersion};
    if (downgradeBackCompat) {
        downgradeOptions.downgradeBackCompat = "";
        downgradeOptions.removeOptions = ["upgradeBackCompat"];
    } else {
        downgradeOptions.removeOptions = ["upgradeBackCompat", "downgradeBackCompat"];
    }

    if (options.downgradeMongos) {
        // Downgrade all mongos hosts if specified
        var numMongoses = this._mongos.length;

        for (var i = 0; i < numMongoses; i++) {
            var mongos = this._mongos[i];
            MongoRunner.stopMongos(mongos);

            mongos = MongoRunner.runMongos({...downgradeOptions, restart: mongos});

            this["s" + i] = this._mongos[i] = mongos;
            if (i == 0)
                this.s = mongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }

    if (options.downgradeShards) {
        // Downgrade shards
        this._rs.forEach((rs) => {
            rs.test.upgradeSet(downgradeOptions);
        });
    }

    if (options.downgradeOneShard) {
        // Downgrade one shard.
        let rs = options.downgradeOneShard;
        rs.upgradeSet(downgradeOptions);
    }

    if (options.downgradeConfigs) {
        this.configRS.upgradeSet(downgradeOptions);
        for (var i = 0; i < this.configRS.nodes.length; i++) {
            this["config" + i] = this.configRS.nodes[i];
            this["c" + i] = this.configRS.nodes[i];
        }
    }

    if (options.waitUntilStable) {
        this.waitUntilStable();
    }
};

ShardingTest.prototype.waitUntilStable = function() {
    // Wait for the config server and shards to become available.
    this.configRS.awaitSecondaryNodes();
    let shardPrimaries = [];
    for (let rs of this._rs) {
        rs.test.awaitSecondaryNodes();
        shardPrimaries.push(rs.test.getPrimary());
    }
    // Wait for the ReplicaSetMonitor on mongoS and each shard to reflect the state of all shards.
    for (let client of [...this._mongos, ...shardPrimaries]) {
        awaitRSClientHosts(client, shardPrimaries, {ok: true, ismaster: true});
    }
};

ShardingTest.prototype.restartMongoses = function() {
    var numMongoses = this._mongos.length;

    for (var i = 0; i < numMongoses; i++) {
        var mongos = this._mongos[i];

        MongoRunner.stopMongos(mongos);
        mongos = MongoRunner.runMongos({restart: mongos});

        this["s" + i] = this._mongos[i] = mongos;
        if (i == 0)
            this.s = mongos;
    }

    this.config = this.s.getDB("config");
    this.admin = this.s.getDB("admin");
};

ShardingTest.prototype.getMongosAtVersion = function(binVersion) {
    var mongoses = this._mongos;
    for (var i = 0; i < mongoses.length; i++) {
        try {
            var version = mongoses[i].getDB("admin").runCommand("serverStatus").version;
            if (version.indexOf(binVersion) == 0) {
                return mongoses[i];
            }
        } catch (e) {
            printjson(e);
            print(mongoses[i]);
        }
    }
};
