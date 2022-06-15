// Merge the two options objects.  Used as a helper when we are trying to actually compare options
// despite the fact that our test framework adds extra stuff to it.  Anything set in the second
// options object overrides the first options object.  The two objects must have the same structure.
function mergeOptions(obj1, obj2) {
    var obj3 = {};
    for (var attrname in obj1) {
        if (typeof obj1[attrname] === "object" && typeof obj2[attrname] !== "undefined") {
            if (typeof obj2[attrname] !== "object") {
                throw Error("Objects being merged must have the same structure");
            }
            obj3[attrname] = mergeOptions(obj1[attrname], obj2[attrname]);
        } else {
            obj3[attrname] = obj1[attrname];
        }
    }
    for (var attrname in obj2) {
        if (typeof obj2[attrname] === "object" && typeof obj1[attrname] !== "undefined") {
            if (typeof obj1[attrname] !== "object") {
                throw Error("Objects being merged must have the same structure");
            }
            // Already handled above
        } else {
            obj3[attrname] = obj2[attrname];
        }
    }
    return obj3;
}

// Test that the parsed result of setting certain command line options has the correct format in
// mongod.  See SERVER-13379.
//
// Arguments:
//   mongoRunnerConfig - Configuration object to pass to the mongo runner
//   expectedResult - Object formatted the same way as the result of running the "getCmdLineOpts"
//      command, but with only the fields that should be set by the options implied by the first
//      argument set.
//
// Example:
//
// testGetCmdLineOptsMongod({ port : 10000 }, { "parsed" : { "net" : { "port" : 10000 } } });
//
var getCmdLineOptsBaseMongod;
function testGetCmdLineOptsMongod(mongoRunnerConfig, expectedResult) {
    // Get the options object returned by "getCmdLineOpts" when we spawn a mongod using our test
    // framework without passing any additional options.  We need this because the framework adds
    // options of its own, and we only want to compare against the options we care about.
    function getBaseOptsObject() {
        // Start mongod with no options
        var baseMongod = MongoRunner.runMongod();

        // Get base command line opts.  Needed because the framework adds its own options
        var getCmdLineOptsBaseMongod = baseMongod.adminCommand("getCmdLineOpts");

        // Stop the mongod we used to get the options
        MongoRunner.stopMongod(baseMongod);

        return getCmdLineOptsBaseMongod;
    }

    if (typeof getCmdLineOptsBaseMongod === "undefined") {
        getCmdLineOptsBaseMongod = getBaseOptsObject();
    }

    // Get base command line opts.  Needed because the framework adds its own options
    var getCmdLineOptsExpected = getCmdLineOptsBaseMongod;

    // Delete port and dbPath if we are not explicitly setting them, since they will change on
    // multiple runs of the test framework and cause false failures.
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.net === "undefined" ||
        typeof expectedResult.parsed.net.port === "undefined") {
        delete getCmdLineOptsExpected.parsed.net.port;
    }
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.storage === "undefined" ||
        typeof expectedResult.parsed.storage.dbPath === "undefined") {
        delete getCmdLineOptsExpected.parsed.storage.dbPath;
    }
    // Delete backtraceLogFile parameter, since we are generating unique value every time
    delete getCmdLineOptsExpected.parsed.setParameter.backtraceLogFile;

    // Merge with the result that we expect
    expectedResult = mergeOptions(getCmdLineOptsExpected, expectedResult);

    // Start mongod with options
    var mongod = MongoRunner.runMongod(mongoRunnerConfig);

    // Create and authenticate high-privilege user in case mongod is running with authorization.
    // Try/catch is necessary in case this is being run on an uninitiated replset, by a test
    // such as repl_options.js for example.
    var ex;
    try {
        mongod.getDB("admin").createUser({user: "root", pwd: "pass", roles: ["root"]});
        mongod.getDB("admin").auth("root", "pass");
    } catch (ex) {
    }

    // Get the parsed options
    var getCmdLineOptsResult = mongod.adminCommand("getCmdLineOpts");

    // Delete port and dbPath if we are not explicitly setting them, since they will change on
    // multiple runs of the test framework and cause false failures.
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.net === "undefined" ||
        typeof expectedResult.parsed.net.port === "undefined") {
        delete getCmdLineOptsResult.parsed.net.port;
    }
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.storage === "undefined" ||
        typeof expectedResult.parsed.storage.dbPath === "undefined") {
        delete getCmdLineOptsResult.parsed.storage.dbPath;
    }
    // Delete backtraceLogFile parameter, since we are generating unique value every time
    delete getCmdLineOptsResult.parsed.setParameter.backtraceLogFile;

    // Make sure the options are equal to what we expect
    assert.docEq(getCmdLineOptsResult.parsed, expectedResult.parsed);

    // Cleanup
    mongod.getDB("admin").logout();
    MongoRunner.stopMongod(mongod);
}

// Test that the parsed result of setting certain command line options has the correct format in
// mongos.  See SERVER-13379.
//
// Arguments:
//   mongoRunnerConfig - Configuration object to pass to the mongo runner
//   expectedResult - Object formatted the same way as the result of running the "getCmdLineOpts"
//      command, but with only the fields that should be set by the options implied by the first
//      argument set.
//
// Example:
//
// testGetCmdLineOptsMongos({ port : 10000 }, { "parsed" : { "net" : { "port" : 10000 } } });
//
var getCmdLineOptsBaseMongos;
function testGetCmdLineOptsMongos(mongoRunnerConfig, expectedResult) {
    "use strict";

    // Get the options object returned by "getCmdLineOpts" when we spawn a mongos using our test
    // framework without passing any additional options.  We need this because the framework adds
    // options of its own, and we only want to compare against the options we care about.
    function getCmdLineOptsFromMongos(mongosOptions) {
        // Start mongod with no options
        var baseMongod =
            MongoRunner.runMongod({configsvr: "", replSet: "csrs", storageEngine: "wiredTiger"});
        assert.commandWorked(baseMongod.adminCommand({
            replSetInitiate:
                {_id: "csrs", configsvr: true, members: [{_id: 0, host: baseMongod.host}]}
        }));
        var configdbStr = "csrs/" + baseMongod.host;
        var ismasterResult;
        assert.soon(
            function() {
                ismasterResult = baseMongod.adminCommand("ismaster");
                return ismasterResult.ismaster;
            },
            function() {
                return tojson(ismasterResult);
            });

        var options = Object.merge(mongosOptions, {configdb: configdbStr});
        var baseMongos = MongoRunner.runMongos(options);

        // Get base command line opts.  Needed because the framework adds its own options
        var getCmdLineOptsResult = baseMongos.adminCommand("getCmdLineOpts");

        // Remove the configdb option
        delete getCmdLineOptsResult.parsed.sharding.configDB;

        // Stop the mongod and mongos we used to get the options
        MongoRunner.stopMongos(baseMongos);
        MongoRunner.stopMongod(baseMongod);

        return getCmdLineOptsResult;
    }

    if (typeof getCmdLineOptsBaseMongos === "undefined") {
        getCmdLineOptsBaseMongos = getCmdLineOptsFromMongos({});
    }

    // Get base command line opts.  Needed because the framework adds its own options
    var getCmdLineOptsExpected = getCmdLineOptsBaseMongos;

    // Delete port if we are not explicitly setting it, since it will change on multiple runs of the
    // test framework and cause false failures.
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.net === "undefined" ||
        typeof expectedResult.parsed.net.port === "undefined") {
        delete getCmdLineOptsExpected.parsed.net.port;
    }

    // Merge with the result that we expect
    expectedResult = mergeOptions(getCmdLineOptsExpected, expectedResult);

    // Get the parsed options
    var getCmdLineOptsResult = getCmdLineOptsFromMongos(mongoRunnerConfig);

    // Delete port if we are not explicitly setting it, since it will change on multiple runs of the
    // test framework and cause false failures.
    if (typeof expectedResult.parsed === "undefined" ||
        typeof expectedResult.parsed.net === "undefined" ||
        typeof expectedResult.parsed.net.port === "undefined") {
        delete getCmdLineOptsResult.parsed.net.port;
    }

    // Make sure the options are equal to what we expect
    assert.docEq(getCmdLineOptsResult.parsed, expectedResult.parsed);
}

// Tests that the passed configuration will not run a new mongod instances. Mainly used to test
// conflicting parameters at startup.
//
// Arguments:
//   mongoRunnerConfig - Configuration object to pass to the mongo runner
// Example:
//
// testGetCmdLineOptsMongodFailed({ shardsvr : "" });
function testGetCmdLineOptsMongodFailed(mongoRunnerConfig) {
    assert.throws(() => MongoRunner.runMongod(mongoRunnerConfig));
}
