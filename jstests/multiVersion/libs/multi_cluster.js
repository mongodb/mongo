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
 *     upgradeMerizos: <bool>, // defaults to true
 * }
 */
ShardingTest.prototype.upgradeCluster = function(binVersion, options) {
    options = options || {};
    if (options.upgradeShards == undefined)
        options.upgradeShards = true;
    if (options.upgradeConfigs == undefined)
        options.upgradeConfigs = true;
    if (options.upgradeMerizos == undefined)
        options.upgradeMerizos = true;

    var upgradedSingleShards = [];

    if (options.upgradeConfigs) {
        // Upgrade config servers if they aren't already upgraded shards
        var numConfigs = this._configServers.length;

        for (var i = 0; i < numConfigs; i++) {
            var configSvr = this._configServers[i];

            if (configSvr.host in upgradedSingleShards) {
                configSvr = upgradedSingleShards[configSvr.host];
            } else {
                MerizoRunner.stopMerizod(configSvr);
                configSvr = MerizoRunner.runMerizod(
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
                MerizoRunner.stopMerizod(shard);
                shard = MerizoRunner.runMerizod(
                    {restart: shard, binVersion: binVersion, appendOptions: true});

                upgradedSingleShards[shard.host] = shard;
                this["shard" + i] = this["d" + i] = this._connections[i] = shard;
            }
        }
    }

    if (options.upgradeMerizos) {
        // Upgrade all merizos hosts if specified
        var numMerizoses = this._merizos.length;

        for (var i = 0; i < numMerizoses; i++) {
            var merizos = this._merizos[i];
            MerizoRunner.stopMerizos(merizos);

            merizos = MerizoRunner.runMerizos(
                {restart: merizos, binVersion: binVersion, appendOptions: true});

            this["s" + i] = this._merizos[i] = merizos;
            if (i == 0)
                this.s = merizos;
        }

        this.config = this.s.getDB("config");
        this.admin = this.s.getDB("admin");
    }
};

ShardingTest.prototype.restartMerizoses = function() {

    var numMerizoses = this._merizos.length;

    for (var i = 0; i < numMerizoses; i++) {
        var merizos = this._merizos[i];

        MerizoRunner.stopMerizos(merizos);
        merizos = MerizoRunner.runMerizos({restart: merizos});

        this["s" + i] = this._merizos[i] = merizos;
        if (i == 0)
            this.s = merizos;
    }

    this.config = this.s.getDB("config");
    this.admin = this.s.getDB("admin");
};

ShardingTest.prototype.getMerizosAtVersion = function(binVersion) {
    var merizoses = this._merizos;
    for (var i = 0; i < merizoses.length; i++) {
        try {
            var version = merizoses[i].getDB("admin").runCommand("serverStatus").version;
            if (version.indexOf(binVersion) == 0) {
                return merizoses[i];
            }
        } catch (e) {
            printjson(e);
            print(merizoses[i]);
        }
    }
};
