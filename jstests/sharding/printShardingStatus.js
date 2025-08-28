// Check that the output from printShardingStatus() (aka sh.status())
// contains important information that it should, like the major section
// headings and the names of sharded collections and their shard keys.

import {ShardingTest} from "jstests/libs/shardingtest.js";

const MONGOS_COUNT = 2;

let st = new ShardingTest({shards: 1, mongos: MONGOS_COUNT});

let standalone = MongoRunner.runMongod();

let mongos = st.s0;
let admin = mongos.getDB("admin");

// Wait for the background thread from the mongos to insert their entries before beginning
// the tests.
assert.soon(function () {
    return MONGOS_COUNT == mongos.getDB("config").mongos.count();
});

function grabStatusOutput(configdb, verbose) {
    let res = print.captureAllOutput(function () {
        return printShardingStatus(configdb, verbose);
    });
    let output = res.output.join("\n");
    jsTestLog(output);
    return output;
}

function assertPresentInOutput(output, content, what) {
    assert(
        output.includes(content),
        what + ' "' + content + '" NOT present in output of ' + "printShardingStatus() (but it should be)",
    );
}

function assertNotPresentInOutput(output, content, what) {
    assert(
        !output.includes(content),
        what + ' "' + content + '" IS present in output of ' + "printShardingStatus() (but it should not be)",
    );
}

////////////////////////
// Basic tests
////////////////////////

let dbName = "thisIsTheDatabase";
let collName = "thisIsTheCollection";
let shardKeyName = "thisIsTheShardKey";
let nsName = dbName + "." + collName;

assert.commandWorked(admin.runCommand({enableSharding: dbName}));
let key = {};
key[shardKeyName] = 1;
assert.commandWorked(admin.runCommand({shardCollection: nsName, key: key}));

function testBasic(output) {
    assertPresentInOutput(output, "shards:", "section header");
    assertPresentInOutput(output, "databases:", "section header");
    assertPresentInOutput(output, "balancer:", "section header");
    assertPresentInOutput(output, "active mongoses:", "section header");
    assertNotPresentInOutput(output, "most recently active mongoses:", "section header");

    assertPresentInOutput(output, dbName, "database");
    assertPresentInOutput(output, collName, "collection");
    assertPresentInOutput(output, shardKeyName, "shard key");
}

function testBasicNormalOnly(output) {
    assertPresentInOutput(output, tojson(version) + " : 2\n", "active mongos version");
}

function testBasicVerboseOnly(output) {
    assertPresentInOutput(output, '"mongoVersion" : ' + tojson(version), "active mongos version");
    assertPresentInOutput(output, '"_id" : ' + tojson(s1Host), "active mongos hostname");
    assertPresentInOutput(output, '"_id" : ' + tojson(s2Host), "active mongos hostname");
}

let buildinfo = assert.commandWorked(mongos.adminCommand("buildinfo"));
let serverStatus1 = assert.commandWorked(mongos.adminCommand("serverStatus"));
let serverStatus2 = assert.commandWorked(st.s1.adminCommand("serverStatus"));
var version = buildinfo.version;
var s1Host = serverStatus1.host;
var s2Host = serverStatus2.host;

// Normal, active mongoses
let outputNormal = grabStatusOutput(st.config, false);
testBasic(outputNormal);
testBasicNormalOnly(outputNormal);

let outputVerbose = grabStatusOutput(st.config, true);
testBasic(outputVerbose);
testBasicVerboseOnly(outputVerbose);

// Take a copy of the config db, in order to test the harder-to-setup cases below.
// Copy into a standalone to also test running printShardingStatus() against a config dump.
let config = mongos.getDB("config");
let configCopy = standalone.getDB("configCopy");
config.getCollectionInfos().forEach(function (c) {
    // It's illegal to copy the system collections.
    if (
        [
            "system.indexBuilds",
            "system.preimages",
            "system.change_collection",
            "cache.chunks.config.system.sessions",
            "system.sessions",
            "system.sharding_ddl_coordinators",
        ].includes(c.name)
    ) {
        return;
    }

    // Create collection with options.
    assert.commandWorked(configCopy.createCollection(c.name, c.options));
    // Clone the docs.
    config
        .getCollection(c.name)
        .find()
        .hint({_id: 1})
        .forEach(function (d) {
            assert.commandWorked(configCopy.getCollection(c.name).insert(d));
        });
    // Build the indexes.
    config
        .getCollection(c.name)
        .getIndexes()
        .forEach(function (i) {
            let key = i.key;
            delete i.key;
            delete i.ns;
            delete i.v;
            assert.commandWorked(configCopy.getCollection(c.name).createIndex(key, i));
        });
});

// Inactive mongoses
// Make the first ping be older than now by 1 second more than the threshold
// Make the second ping be older still by the same amount again
let pingAdjustMs = 60000 + 1000;
let then = new Date();
then.setTime(then.getTime() - pingAdjustMs);
configCopy.mongos.update({_id: s1Host}, {$set: {ping: then}});
then.setTime(then.getTime() - pingAdjustMs);
configCopy.mongos.update({_id: s2Host}, {$set: {ping: then}});

let output = grabStatusOutput(configCopy, false);
assertPresentInOutput(output, "most recently active mongoses:", "section header");
assertPresentInOutput(output, tojson(version) + " : 1\n", "recent mongos version");

output = grabStatusOutput(configCopy, true);
assertPresentInOutput(output, "most recently active mongoses:", "section header");
assertPresentInOutput(output, '"_id" : ' + tojson(s1Host), "recent mongos hostname");
assertNotPresentInOutput(output, '"_id" : ' + tojson(s2Host), "old mongos hostname");

// Older mongoses
configCopy.mongos.remove({_id: s1Host});

output = grabStatusOutput(configCopy, false);
assertPresentInOutput(output, "most recently active mongoses:", "section header");
assertPresentInOutput(output, tojson(version) + " : 1\n", "recent mongos version");

output = grabStatusOutput(configCopy, true);
assertPresentInOutput(output, "most recently active mongoses:", "section header");
assertNotPresentInOutput(output, '"_id" : ' + tojson(s1Host), "removed mongos hostname");
assertPresentInOutput(output, '"_id" : ' + tojson(s2Host), "recent mongos hostname");

// No mongoses at all
configCopy.mongos.remove({});

output = grabStatusOutput(configCopy, false);
assertPresentInOutput(output, "most recently active mongoses:\n        none", "no mongoses");

output = grabStatusOutput(configCopy, true);
assertPresentInOutput(output, "most recently active mongoses:\n        none", "no mongoses (verbose)");

assert(mongos.getDB(dbName).dropDatabase());

////////////////////////
// Extended tests
////////////////////////

let testCollDetailsNum = 0;
function testCollDetails(args) {
    if (args === undefined || typeof args != "object") {
        args = {};
    }

    let getCollName = function (x) {
        return "test.test" + x.zeroPad(4);
    };
    let collName = getCollName(testCollDetailsNum);

    let cmdObj = {shardCollection: collName, key: {_id: 1}};
    if (args.unique) {
        cmdObj.unique = true;
    }
    assert.commandWorked(admin.runCommand(cmdObj));

    let originalValues = {};
    if (args.hasOwnProperty("unique")) {
        originalValues["unique"] = mongos.getDB("config").collections.findOne({_id: collName}).unique;
        assert.commandWorked(
            mongos.getDB("config").collections.update({_id: collName}, {$set: {"unique": args.unique}}),
        );
    }
    if (args.hasOwnProperty("noBalance")) {
        originalValues["noBalance"] = mongos.getDB("config").collections.findOne({_id: collName}).noBalance;
        assert.commandWorked(
            mongos.getDB("config").collections.update({_id: collName}, {$set: {"noBalance": args.noBalance}}),
        );
    }

    let output = grabStatusOutput(st.config);

    assertPresentInOutput(output, collName, "collection");

    // If any of the previous collection names are present, then their optional indicators
    // might also be present.  This might taint the results when we go searching through
    // the output.
    // This also means that earlier collNames can't be a prefix of later collNames.
    for (let i = 0; i < testCollDetailsNum; i++) {
        assertNotPresentInOutput(output, getCollName(i), "previous collection");
    }

    assertPresentInOutput(output, "unique: " + !!args.unique, "unique shard key indicator");

    if (args.hasOwnProperty("unique") && typeof args.unique != "boolean") {
        // non-bool: actual value must be shown
        assertPresentInOutput(output, tojson(args.unique), "unique shard key indicator (non bool)");

        // Restore original value.
        assert.commandWorked(
            mongos.getDB("config").collections.update({_id: collName}, {$set: {"unique": originalValues.unique}}),
        );
    }

    assertPresentInOutput(output, "balancing: " + !args.noBalance, "balancing indicator (inverse of noBalance)");
    if (args.hasOwnProperty("noBalance") && typeof args.noBalance != "boolean") {
        // non-bool: actual value must be shown
        assertPresentInOutput(output, tojson(args.noBalance), "noBalance indicator (non bool)");

        // Restore original value.
        assert.commandWorked(
            mongos.getDB("config").collections.update({_id: collName}, {$set: {"noBalance": originalValues.noBalance}}),
        );
    }

    try {
        mongos.getCollection(collName).drop();
    } catch (e) {
        // Ignore drop errors because they are from the illegal values in the collection entry
        assert.commandWorked(mongos.getDB("config").collections.remove({_id: collName}));
    }

    testCollDetailsNum++;
}

assert.commandWorked(admin.runCommand({enableSharding: "test"}));

// Defaults
testCollDetails({});

// Expected values
testCollDetails({unique: false, noBalance: false});
testCollDetails({unique: true, noBalance: true});

// Unexpected truthy values
testCollDetails({unique: "truthy unique value 1", noBalance: "truthy noBalance value 1"});
testCollDetails({unique: 1, noBalance: 1});
testCollDetails({unique: -1, noBalance: -1});
testCollDetails({unique: {}, noBalance: {}});

// Unexpected falsy values
testCollDetails({unique: "", noBalance: ""});
testCollDetails({unique: 0, noBalance: 0});

assert(mongos.getDB("test").dropDatabase());

MongoRunner.stopMongod(standalone);

st.stop();
