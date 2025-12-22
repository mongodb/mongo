//
// MultiVersion utility functions for clusters
//

import "jstests/multiVersion/libs/multi_rs.js";

import {copyJSON} from "jstests/libs/json_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {awaitRSClientHosts} from "jstests/replsets/rslib.js";

/**
 * Restarts the specified binaries in upgradeOptions with the specified binVersion.
 * Note: this does not perform any upgrade operations.
 *
 * @param binVersion {string}
 * @param upgradeOptions {Object} format:
 *
 * {
 *     upgradeShards: <bool>, // defaults to true
 *     upgradeOneShard: <rs> // defaults to false,
 *     upgradeConfigs: <bool>, // defaults to true
 *     upgradeOneConfigNode: <number> // defaults to -1, index of config server node to upgrade, -1 means no single node upgrade
 *     upgradeMongos: <bool>, // defaults to true
 *     waitUntilStable: <bool>, // defaults to false since it provides a more realistic
 *                                 approximation of real-world upgrade behaviour, even though
 *                                 certain tests will likely want a stable cluster after upgrading.
 * }
 */
ShardingTest.prototype.upgradeCluster = function (binVersion, upgradeOptions, nodeOptions = {}) {
    upgradeOptions = upgradeOptions || {};
    if (upgradeOptions.upgradeShards == undefined) upgradeOptions.upgradeShards = true;
    if (upgradeOptions.upgradeOneShard == undefined) upgradeOptions.upgradeOneShard = false;
    if (upgradeOptions.upgradeConfigs == undefined) upgradeOptions.upgradeConfigs = true;
    if (upgradeOptions.upgradeOneConfigNode == undefined) upgradeOptions.upgradeOneConfigNode = -1;
    if (upgradeOptions.upgradeMongos == undefined) upgradeOptions.upgradeMongos = true;
    if (upgradeOptions.waitUntilStable == undefined) upgradeOptions.waitUntilStable = false;

    if (upgradeOptions.upgradeConfigs) {
        // Upgrade config servers
        const numConfigs = this.configRS.nodes.length;

        for (var i = 0; i < numConfigs; i++) {
            let configSvr = this.configRS.nodes[i];

            MongoRunner.stopMongod(configSvr);
            // Must copy the nodeOptions since they are modified by callee.
            const configSrvOptions = copyJSON(nodeOptions);
            configSvr = MongoRunner.runMongod({
                restart: configSvr,
                binVersion: binVersion,
                appendOptions: true,
                ...configSrvOptions,
            });

            this["config" + i] = this["c" + i] = this.configRS.nodes[i] = configSvr;
        }
    } else if (upgradeOptions.upgradeOneConfigNode >= 0) {
        // Upgrade one specific config server node by index
        jsTest.log.info(`Upgrading config server node index: ${upgradeOptions.upgradeOneConfigNode}`);
        const nodeIndex = upgradeOptions.upgradeOneConfigNode;
        assert(nodeIndex < this.configRS.nodes.length);
        let configNode = this.configRS.nodes[nodeIndex];

        MongoRunner.stopMongod(configNode);
        // Must copy the nodeOptions since they are modified by callee.
        const configSvrOptions = copyJSON(nodeOptions);
        configNode = MongoRunner.runMongod({
            restart: configNode,
            binVersion: binVersion,
            appendOptions: true,
            ...configSvrOptions,
        });
        this.configRS.awaitSecondaryNodes(this.configRS.timeoutMS, [configNode]);

        jsTest.log.info(`Upgraded config server node index: ${upgradeOptions.upgradeOneConfigNode}, ${configNode}`);

        // Update the reference in the configRS nodes array and shortcuts
        this["config" + nodeIndex] = this["c" + nodeIndex] = this.configRS.nodes[nodeIndex] = configNode;

        this.configRS.awaitNodesAgreeOnPrimary();
        jsTest.log.info(`Config node ${nodeIndex} successfully restarted and rejoined`);
    }

    if (upgradeOptions.upgradeShards) {
        // Upgrade shards
        this._rs.forEach((rs) => {
            // Must copy the nodeOptions since they are modified by callee.
            const replSetOptions = copyJSON(nodeOptions);
            rs.test.upgradeSet({binVersion: binVersion, ...replSetOptions});
        });
    }

    if (upgradeOptions.upgradeOneShard) {
        // Upgrade one shard.
        let rs = upgradeOptions.upgradeOneShard;
        // Must copy the nodeOptions since they are modified by callee.
        const replSetOptions = copyJSON(nodeOptions);
        rs.upgradeSet({binVersion: binVersion, ...replSetOptions});
    }

    if (upgradeOptions.upgradeMongos) {
        // Upgrade all mongos hosts if specified
        let numMongoses = this._mongos.length;

        for (var i = 0; i < numMongoses; i++) {
            let mongos = this._mongos[i];
            MongoRunner.stopMongos(mongos);

            // Must copy the nodeOptions since they are modified by callee.
            const mongosOptions = copyJSON(nodeOptions);
            mongos = MongoRunner.runMongos({
                restart: mongos,
                binVersion: binVersion,
                appendOptions: true,
                ...mongosOptions,
            });

            this["s" + i] = this._mongos[i] = mongos;
            if (i == 0) this.s = mongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }

    if (upgradeOptions.waitUntilStable) {
        this.waitUntilStable();
    }
};

ShardingTest.prototype.downgradeCluster = function (binVersion, downgradeOptions, nodeOptions = {}) {
    downgradeOptions = downgradeOptions || {};
    if (downgradeOptions.downgradeShards == undefined) downgradeOptions.downgradeShards = true;
    if (downgradeOptions.downgradeOneShard == undefined) downgradeOptions.downgradeOneShard = false;
    if (downgradeOptions.downgradeConfigs == undefined) downgradeOptions.downgradeConfigs = true;
    if (downgradeOptions.downgradeOneConfig == undefined) downgradeOptions.downgradeOneConfig = -1;
    if (downgradeOptions.downgradeMongos == undefined) downgradeOptions.downgradeMongos = true;
    if (downgradeOptions.waitUntilStable == undefined) downgradeOptions.waitUntilStable = false;

    if (downgradeOptions.downgradeMongos) {
        // Downgrade all mongos hosts if specified
        let numMongoses = this._mongos.length;

        for (var i = 0; i < numMongoses; i++) {
            let mongos = this._mongos[i];
            MongoRunner.stopMongos(mongos);

            // Must copy the nodeOptions since they are modified by callee.
            const mongosOptions = copyJSON(nodeOptions);
            mongos = MongoRunner.runMongos({
                restart: mongos,
                binVersion: binVersion,
                appendOptions: true,
                ...mongosOptions,
            });

            this["s" + i] = this._mongos[i] = mongos;
            if (i == 0) this.s = mongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }

    if (downgradeOptions.downgradeShards) {
        // Downgrade shards
        this._rs.forEach((rs) => {
            // Must copy the nodeOptions since they are modified by callee.
            const replSetOptions = copyJSON(nodeOptions);
            rs.test.upgradeSet({binVersion: binVersion, ...replSetOptions});
        });
    }

    if (downgradeOptions.downgradeOneShard) {
        // Must copy the nodeOptions since they are modified by callee.
        const replSetOptions = copyJSON(nodeOptions);

        // Downgrade one shard.
        let rs = downgradeOptions.downgradeOneShard;
        rs.upgradeSet({binVersion: binVersion, ...replSetOptions});
    }

    if (downgradeOptions.downgradeConfigs) {
        // Downgrade config servers
        const numConfigs = this.configRS.nodes.length;

        for (var i = 0; i < numConfigs; i++) {
            let configSvr = this.configRS.nodes[i];

            // Must copy the nodeOptions since they are modified by callee.
            const configSvrOptions = copyJSON(nodeOptions);
            MongoRunner.stopMongod(configSvr);
            configSvr = MongoRunner.runMongod({
                restart: configSvr,
                binVersion: binVersion,
                appendOptions: true,
                ...configSvrOptions,
            });

            this["config" + i] = this["c" + i] = this.configRS.nodes[i] = configSvr;
        }
    } else if (downgradeOptions.downgradeOneConfig >= 0) {
        // Downgrade one specific config server node by index
        const nodeIndex = downgradeOptions.downgradeOneConfig;
        assert(nodeIndex < this.configRS.nodes.length);
        let configNode = this.configRS.nodes[nodeIndex];

        // Must copy the nodeOptions since they are modified by callee.
        const configSvrOptions = copyJSON(nodeOptions);
        MongoRunner.stopMongod(configNode);
        configNode = MongoRunner.runMongod({
            restart: configNode,
            binVersion: binVersion,
            appendOptions: true,
            ...configSvrOptions,
        });
        this.configRS.awaitSecondaryNodes(this.configRS.timeoutMS, [configNode]);

        jsTest.log.info(`Downgraded config server node index: ${downgradeOptions.downgradeOneConfig}, ${configNode}`);

        // Update the reference in the configRS nodes array and shortcuts
        this["config" + nodeIndex] = this["c" + nodeIndex] = this.configRS.nodes[nodeIndex] = configNode;

        this.configRS.awaitNodesAgreeOnPrimary();
        jsTest.log.info(`Config node ${nodeIndex} successfully restarted and rejoined`);
    }

    if (downgradeOptions.waitUntilStable) {
        this.waitUntilStable();
    }
};

ShardingTest.prototype.waitUntilStable = function () {
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

ShardingTest.prototype.restartMongoses = function () {
    let numMongoses = this._mongos.length;

    for (let i = 0; i < numMongoses; i++) {
        let mongos = this._mongos[i];

        MongoRunner.stopMongos(mongos);
        mongos = MongoRunner.runMongos({restart: mongos});

        this["s" + i] = this._mongos[i] = mongos;
        if (i == 0) this.s = mongos;
    }

    this.config = this.s.getDB("config");
    this.admin = this.s.getDB("admin");
};

ShardingTest.prototype.getMongosAtVersion = function (binVersion) {
    let mongoses = this._mongos;
    for (let i = 0; i < mongoses.length; i++) {
        try {
            let version = mongoses[i].getDB("admin").runCommand("serverStatus").version;
            if (version.indexOf(binVersion) == 0) {
                return mongoses[i];
            }
        } catch (e) {
            printjson(e);
            print(mongoses[i]);
        }
    }
};
