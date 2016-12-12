var StandaloneFixture, ShardedFixture, runReadOnlyTest, zip2, cycleN;

(function() {
    "use strict";

    function makeDirectoryReadOnly(dir) {
        if (_isWindows()) {
            run("attrib", "+r", dir + "\\*.*", "/s");
        } else {
            run("chmod", "-R", "a-w", dir);
        }
    }

    function makeDirectoryWritable(dir) {
        if (_isWindows()) {
            run("attrib", "-r", dir + "\\*.*", "/s");
        } else {
            run("chmod", "-R", "a+w", dir);
        }
    }

    StandaloneFixture = function() {};

    StandaloneFixture.prototype.runLoadPhase = function runLoadPhase(test) {
        this.mongod = MongoRunner.runMongod({});
        this.dbpath = this.mongod.dbpath;

        test.load(this.mongod.getDB("test")[test.name]);
        assert.commandWorked(this.mongod.getDB("local").dropDatabase());
        MongoRunner.stopMongod(this.mongod);
    };

    StandaloneFixture.prototype.runExecPhase = function runExecPhase(test) {
        try {
            makeDirectoryReadOnly(this.dbpath);

            var options = {queryableBackupMode: "", noCleanData: true, dbpath: this.dbpath};

            this.mongod = MongoRunner.runMongod(options);

            test.exec(this.mongod.getDB("test")[test.name]);

            MongoRunner.stopMongod(this.mongod);
        } finally {
            makeDirectoryWritable(this.dbpath);
        }
    };

    ShardedFixture = function() {
        this.nShards = 3;
    };

    ShardedFixture.prototype.runLoadPhase = function runLoadPhase(test) {
        this.shardingTest = new ShardingTest({nopreallocj: true, mongos: 1, shards: this.nShards});

        this.paths = this.shardingTest.getDBPaths();

        jsTest.log("sharding test collection...");

        // Use a hashed shard key so we actually hit multiple shards.
        this.shardingTest.shardColl(test.name, {_id: "hashed"});

        test.load(this.shardingTest.getDB("test")[test.name]);
    };

    ShardedFixture.prototype.runExecPhase = function runExecPhase(test) {
        jsTest.log("restarting shards...");
        try {
            for (var i = 0; i < this.nShards; ++i) {
                // Write the shard's shardIdentity to a config file under
                // sharding._overrideShardIdentity, since the shardIdentity must be provided through
                // overrideShardIdentity when running in queryableBackupMode, and is only allowed to
                // be set via config file.

                var shardIdentity = this.shardingTest["d" + i]
                                        .getDB("admin")
                                        .getCollection("system.version")
                                        .findOne({_id: "shardIdentity"});
                assert.neq(null, shardIdentity);

                // Construct a string representation of the config file (replace all instances of
                // multiple consecutive whitespace characters in the string representation of the
                // shardIdentity JSON document, including newlines, with single white spaces).
                var configFileStr = "sharding:\n  _overrideShardIdentity: '" +
                    tojson(shardIdentity).replace(/\s+/g, ' ') + "'";

                // Use the os-specific path delimiter.
                var delim = _isWindows() ? '\\' : '/';
                var configFilePath = this.paths[i] + delim + "config-for-shard-" + i + ".yml";

                writeFile(configFilePath, configFileStr);

                var opts = {
                    config: configFilePath,
                    queryableBackupMode: "",
                    shardsvr: "",
                    dbpath: this.paths[i]
                };

                assert.commandWorked(this.shardingTest["d" + i].getDB("local").dropDatabase());
                this.shardingTest.restartMongod(i, opts, () => {
                    makeDirectoryReadOnly(this.paths[i]);
                });
            }

            jsTest.log("restarting mongos...");

            this.shardingTest.restartMongos(0);

            test.exec(this.shardingTest.getDB("test")[test.name]);

            this.paths.forEach((path) => {
                makeDirectoryWritable(path);
            });

            this.shardingTest.stop();
        } finally {
            this.paths.forEach((path) => {
                makeDirectoryWritable(path);
            });
        }
    };

    runReadOnlyTest = function(test) {
        printjson(test);

        assert.eq(typeof(test.exec), "function");
        assert.eq(typeof(test.load), "function");
        assert.eq(typeof(test.name), "string");

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

    cycleN = function * (arr, N) {
        for (var i = 0; i < N; ++i) {
            yield arr[i % arr.length];
        }
    };

    zip2 = function * (iter1, iter2) {
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
