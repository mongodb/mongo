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
// merizod.  See SERVER-13379.
//
// Arguments:
//   merizoRunnerConfig - Configuration object to pass to the merizo runner
//   expectedResult - Object formatted the same way as the result of running the "getCmdLineOpts"
//      command, but with only the fields that should be set by the options implied by the first
//      argument set.
//
// Example:
//
// testGetCmdLineOptsMongod({ port : 10000 }, { "parsed" : { "net" : { "port" : 10000 } } });
//
var getCmdLineOptsBaseMongod;
function testGetCmdLineOptsMongod(merizoRunnerConfig, expectedResult) {
    // Get the options object returned by "getCmdLineOpts" when we spawn a merizod using our test
    // framework without passing any additional options.  We need this because the framework adds
    // options of its own, and we only want to compare against the options we care about.
    function getBaseOptsObject() {
        // Start merizod with no options
        var baseMongod = MongoRunner.runMongod();

        // Get base command line opts.  Needed because the framework adds its own options
        var getCmdLineOptsBaseMongod = baseMongod.adminCommand("getCmdLineOpts");

        // Stop the merizod we used to get the options
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

    // Merge with the result that we expect
    expectedResult = mergeOptions(getCmdLineOptsExpected, expectedResult);

    // Start merizod with options
    var merizod = MongoRunner.runMongod(merizoRunnerConfig);

    // Create and authenticate high-privilege user in case merizod is running with authorization.
    // Try/catch is necessary in case this is being run on an uninitiated replset, by a test
    // such as repl_options.js for example.
    var ex;
    try {
        merizod.getDB("admin").createUser({user: "root", pwd: "pass", roles: ["root"]});
        merizod.getDB("admin").auth("root", "pass");
    } catch (ex) {
    }

    // Get the parsed options
    var getCmdLineOptsResult = merizod.adminCommand("getCmdLineOpts");

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

    // Make sure the options are equal to what we expect
    assert.docEq(getCmdLineOptsResult.parsed, expectedResult.parsed);

    // Cleanup
    merizod.getDB("admin").logout();
    MongoRunner.stopMongod(merizod);
}

// Test that the parsed result of setting certain command line options has the correct format in
// merizos.  See SERVER-13379.
//
// Arguments:
//   merizoRunnerConfig - Configuration object to pass to the merizo runner
//   expectedResult - Object formatted the same way as the result of running the "getCmdLineOpts"
//      command, but with only the fields that should be set by the options implied by the first
//      argument set.
//
// Example:
//
// testGetCmdLineOptsMongos({ port : 10000 }, { "parsed" : { "net" : { "port" : 10000 } } });
//
var getCmdLineOptsBaseMongos;
function testGetCmdLineOptsMongos(merizoRunnerConfig, expectedResult) {
    "use strict";

    // Get the options object returned by "getCmdLineOpts" when we spawn a merizos using our test
    // framework without passing any additional options.  We need this because the framework adds
    // options of its own, and we only want to compare against the options we care about.
    function getCmdLineOptsFromMongos(merizosOptions) {
        // Start merizod with no options
        var baseMongod = MongoRunner.runMongod(
            {configsvr: "", journal: "", replSet: "csrs", storageEngine: "wiredTiger"});
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

        var options = Object.merge(merizosOptions, {configdb: configdbStr});
        var baseMongos = MongoRunner.runMongos(options);

        // Get base command line opts.  Needed because the framework adds its own options
        var getCmdLineOptsResult = baseMongos.adminCommand("getCmdLineOpts");

        // Remove the configdb option
        delete getCmdLineOptsResult.parsed.sharding.configDB;

        // Stop the merizod and merizos we used to get the options
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
    var getCmdLineOptsResult = getCmdLineOptsFromMongos(merizoRunnerConfig);

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
