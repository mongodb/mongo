var StandaloneFixture, ShardedFixture, runReadOnlyTest, zip2, cycleN;

(function() {
"use strict";

StandaloneFixture = function() {};

StandaloneFixture.prototype.runLoadPhase = function runLoadPhase(test) {
    this.mongod = MongoRunner.runMongod({});
    this.dbpath = this.mongod.dbpath;

    test.load(this.mongod.getDB("test")[test.name]);
    assert.commandWorked(this.mongod.getDB("local").dropDatabase());
    MongoRunner.stopMongod(this.mongod);
};

StandaloneFixture.prototype.runExecPhase = function runExecPhase(test) {
    var options = {queryableBackupMode: "", noCleanData: true, dbpath: this.dbpath};
    this.mongod = MongoRunner.runMongod(options);
    assert.neq(this.mongod, null);
    test.exec(this.mongod.getDB("test")[test.name]);
    MongoRunner.stopMongod(this.mongod);
};

ShardedFixture = function() {
    this.nShards = 3;
    this.nConfigs = 3;
};

ShardedFixture.prototype.runLoadPhase = function runLoadPhase(test) {
    this.st =
        new ShardingTest({mongos: 1, config: this.nConfigs, shards: this.nShards, rs: {nodes: 1}});

    // This information will be needed later on to set up the shards on read only mode.
    this.dbPaths = [];
    this.ports = [];
    this.hosts = [];
    for (let i = 0; i < this.nShards; ++i) {
        let primary = this.st["rs" + i].getPrimary();
        this.dbPaths.push(primary.dbpath);
        this.ports.push(primary.port);
        this.hosts.push(primary.host);
    }

    jsTest.log("sharding test collection...");

    // Use a hashed shard key so we actually hit multiple shards.
    this.st.shardColl(test.name, {_id: "hashed"}, false);

    test.load(this.st.getDB("test")[test.name]);
};

ShardedFixture.prototype.runExecPhase = function runExecPhase(test) {
    const docs = [{_id: 1}];
    let shardIdentities = [];
    let readOnlyShards = [];

    for (let i = 0; i < this.nShards; ++i) {
        let primary = this.st["rs" + i].getPrimary();
        shardIdentities.push(
            primary.getDB("admin").getCollection("system.version").findOne({_id: "shardIdentity"}));
        assert.neq(null, shardIdentities[i]);
        assert.commandWorked(primary.getDB('_temporary_db').runCommand({
            insert: '_temporary_coll' + i,
            documents: docs,
            writeConcern: {w: "majority"}
        }));
    }

    this.st.stopAllShards({noCleanData: true, restart: true, skipValidation: true});

    jsTest.log("Restarting shards as read only standalone instances...");
    for (let i = 0; i < this.nShards; ++i) {
        let dbPath = this.dbPaths[i];
        let port = this.ports[i];

        jsTestLog("Renaming local.system collection on shard " + i);

        let tempMongod =
            MongoRunner.runMongod({port: port, dbpath: dbPath, noReplSet: true, noCleanData: true});
        // Rename the local.system collection to prevent problems with replset configurations.
        tempMongod.getDB('local').getCollection('system').renameCollection('_system');
        MongoRunner.stopMongod(
            tempMongod, null, {noCleanData: true, skipValidation: true, wait: true});

        let shardIdentity = shardIdentities[i];
        let host = this.hosts[i];

        // Change the connection string format so the router can create a standalone targeter
        // instead of a replica set targeter.
        this.st.config.shards.update({_id: shardIdentity.shardName}, {$set: {host: host}});

        // Restart the shard as standalone on read only mode.
        let configFileStr = "sharding:\n _overrideShardIdentity: '" +
            tojson(shardIdentity).replace(/\s+/g, ' ') + "'";
        // Use the os-specific path delimiter.
        jsTestLog('Seting up shard ' + i + ' with new shard identity ' + tojson(shardIdentity));
        let delim = _isWindows() ? '\\' : '/';
        let configFilePath = dbPath + delim + "config-for-shard-" + i + ".yml";

        writeFile(configFilePath, configFileStr);

        readOnlyShards.push(MongoRunner.runMongod({
            config: configFilePath,
            dbpath: dbPath,
            port: port,
            queryableBackupMode: "",
            restart: true,
            shardsvr: "",
        }));
    }

    // Restart the config server  and the router so they can reload the shard registry.
    jsTest.log("Restarting the config server...");
    for (let i = 0; i < this.nConfig; ++i) {
        this.st.restartConfigServer(i);
    }

    jsTest.log("Restarting mongos...");
    this.st.restartMongos(0);
    test.exec(this.st.getDB("test")[test.name]);

    for (let i = 0; i < this.nShards; ++i) {
        let dbPath = this.dbPaths[i];
        let port = this.ports[i];

        // Stop the read only shards.
        MongoRunner.stopMongod(
            readOnlyShards[i], null, {noCleanData: true, skipValidation: true, wait: true});

        // Run a temporary mongod to rename the local.system collection.
        let tempMongod =
            MongoRunner.runMongod({port: port, dbpath: dbPath, noReplSet: true, noCleanData: true});
        tempMongod.getDB('local').getCollection('_system').renameCollection('system', true);
        MongoRunner.stopMongod(
            tempMongod, null, {noCleanData: true, skipValidation: true, wait: true});

        let shardIdentity = shardIdentities[i];
        let host = this.hosts[i];
        this.st.config.shards.update({_id: shardIdentity.shardName},
                                     {$set: {host: shardIdentity.shardName + '/' + host}});

        // Restart the shard to have a well behaved departure.
        this.st.restartShardRS(i);
    }

    // Restart the config server and the router so the shard registry refreshes and enable the RSM
    // again.
    for (let i = 0; i < this.nConfigs; ++i) {
        this.st.restartConfigServer(i);
    }

    this.st.restartMongos(0);

    this.st.stop();
};

runReadOnlyTest = function(test) {
    printjson(test);

    assert.eq(typeof (test.exec), "function");
    assert.eq(typeof (test.load), "function");
    assert.eq(typeof (test.name), "string");

    var fixtureType = TestData.fixture || "standalone";

    var fixture = null;
    if (fixtureType === "standalone") {
        fixture = new StandaloneFixture();
    } else if (fixtureType === "sharded") {
        fixture = new ShardedFixture();
    } else {
        throw new Error("fixtureType must be one of either 'standalone' or 'sharded'");
    }

    jsTest.log("starting load phase for test: " + test.name);
    fixture.runLoadPhase(test);

    jsTest.log("starting execution phase for test: " + test.name);
    fixture.runExecPhase(test);
};

cycleN = function*(arr, N) {
    for (var i = 0; i < N; ++i) {
        yield arr[i % arr.length];
    }
};

zip2 = function*(iter1, iter2) {
    var n1 = iter1.next();
    var n2 = iter2.next();
    while (!n1.done || !n2.done) {
        var res = [];
        if (!n1.done) {
            res.push(n1.value);
            n1 = iter1.next();
        }
        if (!n2.done) {
            res.push(n2.value);
            n2 = iter2.next();
        }

        yield res;
    }
};
}());
