//
// MultiVersion utility functions for clusters
//

/**
 * Restarts the specified binaries in options with the specified binVersion.
 * Note: this does not perform any upgrade operations.
 *
 * @param binVersion {string}
 * @param options {Object} format:
 *
 * {
 *     upgradeShards: <bool>, // defaults to true
 *     upgradeConfigs: <bool>, // defaults to true
 *     upgradeBongos: <bool>, // defaults to true
 * }
 */
ShardingTest.prototype.upgradeCluster = function(binVersion, options) {
    options = options || {};
    if (options.upgradeShards == undefined)
        options.upgradeShards = true;
    if (options.upgradeConfigs == undefined)
        options.upgradeConfigs = true;
    if (options.upgradeBongos == undefined)
        options.upgradeBongos = true;

    var upgradedSingleShards = [];

    if (options.upgradeConfigs) {
        // Upgrade config servers if they aren't already upgraded shards
        var numConfigs = this._configServers.length;

        for (var i = 0; i < numConfigs; i++) {
            var configSvr = this._configServers[i];

            if (configSvr.host in upgradedSingleShards) {
                configSvr = upgradedSingleShards[configSvr.host];
            } else {
                BongoRunner.stopBongod(configSvr);
                configSvr = BongoRunner.runBongod(
                    {restart: configSvr, binVersion: binVersion, appendOptions: true});
            }

            this["config" + i] = this["c" + i] = this._configServers[i] = configSvr;
        }
    }

    if (options.upgradeShards) {
        var numShards = this._connections.length;

        // Upgrade shards
        for (var i = 0; i < numShards; i++) {
            if (this._rs && this._rs[i]) {
                // Upgrade replica set
                var rst = this._rs[i].test;
                rst.upgradeSet({binVersion: binVersion});
            } else {
                // Upgrade shard
                var shard = this._connections[i];
                BongoRunner.stopBongod(shard);
                shard = BongoRunner.runBongod(
                    {restart: shard, binVersion: binVersion, appendOptions: true});

                upgradedSingleShards[shard.host] = shard;
                this["shard" + i] = this["d" + i] = this._connections[i] = shard;
            }
        }
    }

    if (options.upgradeBongos) {
        // Upgrade all bongos hosts if specified
        var numBongoses = this._bongos.length;

        for (var i = 0; i < numBongoses; i++) {
            var bongos = this._bongos[i];
            BongoRunner.stopBongos(bongos);

            bongos = BongoRunner.runBongos(
                {restart: bongos, binVersion: binVersion, appendOptions: true});

            this["s" + i] = this._bongos[i] = bongos;
            if (i == 0)
                this.s = bongos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }
};

ShardingTest.prototype.restartBongoses = function() {

    var numBongoses = this._bongos.length;

    for (var i = 0; i < numBongoses; i++) {
        var bongos = this._bongos[i];

        BongoRunner.stopBongos(bongos);
        bongos = BongoRunner.runBongos({restart: bongos});

        this["s" + i] = this._bongos[i] = bongos;
        if (i == 0)
            this.s = bongos;
    }

    this.config = this.s.getDB("config");
    this.admin = this.s.getDB("admin");
};

ShardingTest.prototype.getBongosAtVersion = function(binVersion) {
    var bongoses = this._bongos;
    for (var i = 0; i < bongoses.length; i++) {
        try {
            var version = bongoses[i].getDB("admin").runCommand("serverStatus").version;
            if (version.indexOf(binVersion) == 0) {
                return bongoses[i];
            }
        } catch (e) {
            printjson(e);
            print(bongoses[i]);
        }
    }
};
