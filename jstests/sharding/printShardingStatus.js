// Check that the output from printShardingStatus() (aka sh.status())
// contains important information that it should, like the major section
// headings and the names of sharded collections and their shard keys.

(function() {
    'use strict';

    var st = new ShardingTest({shards: 1, bongos: 2, config: 1, other: {smallfiles: true}});

    var bongos = st.s0;
    var admin = bongos.getDB("admin");

    function grabStatusOutput(configdb, verbose) {
        var res = print.captureAllOutput(function() {
            return printShardingStatus(configdb, verbose);
        });
        var output = res.output.join("\n");
        jsTestLog(output);
        return output;
    }

    function assertPresentInOutput(output, content, what) {
        assert(output.includes(content),
               what + " \"" + content + "\" NOT present in output of " +
                   "printShardingStatus() (but it should be)");
    }

    function assertNotPresentInOutput(output, content, what) {
        assert(!output.includes(content),
               what + " \"" + content + "\" IS present in output of " +
                   "printShardingStatus() (but it should not be)");
    }

    ////////////////////////
    // Basic tests
    ////////////////////////

    var dbName = "thisIsTheDatabase";
    var collName = "thisIsTheCollection";
    var shardKeyName = "thisIsTheShardKey";
    var nsName = dbName + "." + collName;

    assert.commandWorked(admin.runCommand({enableSharding: dbName}));
    var key = {};
    key[shardKeyName] = 1;
    assert.commandWorked(admin.runCommand({shardCollection: nsName, key: key}));

    function testBasic(output) {
        assertPresentInOutput(output, "shards:", "section header");
        assertPresentInOutput(output, "databases:", "section header");
        assertPresentInOutput(output, "balancer:", "section header");
        assertPresentInOutput(output, "active bongoses:", "section header");
        assertNotPresentInOutput(output, "most recently active bongoses:", "section header");

        assertPresentInOutput(output, dbName, "database");
        assertPresentInOutput(output, collName, "collection");
        assertPresentInOutput(output, shardKeyName, "shard key");
    }

    function testBasicNormalOnly(output) {
        assertPresentInOutput(output, tojson(version) + " : 2\n", "active bongos version");
    }

    function testBasicVerboseOnly(output) {
        assertPresentInOutput(
            output, '"bongoVersion" : ' + tojson(version), "active bongos version");
        assertPresentInOutput(output, '"_id" : ' + tojson(s1Host), "active bongos hostname");
        assertPresentInOutput(output, '"_id" : ' + tojson(s2Host), "active bongos hostname");
    }

    var buildinfo = assert.commandWorked(bongos.adminCommand("buildinfo"));
    var serverStatus1 = assert.commandWorked(bongos.adminCommand("serverStatus"));
    var serverStatus2 = assert.commandWorked(st.s1.adminCommand("serverStatus"));
    var version = buildinfo.version;
    var s1Host = serverStatus1.host;
    var s2Host = serverStatus2.host;

    // Normal, active bongoses
    var outputNormal = grabStatusOutput(st.config, false);
    testBasic(outputNormal);
    testBasicNormalOnly(outputNormal);

    var outputVerbose = grabStatusOutput(st.config, true);
    testBasic(outputVerbose);
    testBasicVerboseOnly(outputVerbose);

    // Take a copy of the config db, in order to test the harder-to-setup cases below.
    // TODO: Replace this manual copy with copydb once SERVER-13080 is fixed.
    var config = bongos.getDB("config");
    var configCopy = bongos.getDB("configCopy");
    config.getCollectionInfos().forEach(function(c) {
        // Create collection with options.
        assert.commandWorked(configCopy.createCollection(c.name, c.options));
        // Clone the docs.
        config.getCollection(c.name).find().snapshot().forEach(function(d) {
            assert.writeOK(configCopy.getCollection(c.name).insert(d));
        });
        // Build the indexes.
        config.getCollection(c.name).getIndexes().forEach(function(i) {
            var key = i.key;
            delete i.key;
            delete i.ns;
            delete i.v;
            assert.commandWorked(configCopy.getCollection(c.name).ensureIndex(key, i));
        });
    });

    // Inactive bongoses
    // Make the first ping be older than now by 1 second more than the threshold
    // Make the second ping be older still by the same amount again
    var pingAdjustMs = 60000 + 1000;
    var then = new Date();
    then.setTime(then.getTime() - pingAdjustMs);
    configCopy.bongos.update({_id: s1Host}, {$set: {ping: then}});
    then.setTime(then.getTime() - pingAdjustMs);
    configCopy.bongos.update({_id: s2Host}, {$set: {ping: then}});

    var output = grabStatusOutput(configCopy, false);
    assertPresentInOutput(output, "most recently active bongoses:", "section header");
    assertPresentInOutput(output, tojson(version) + " : 1\n", "recent bongos version");

    var output = grabStatusOutput(configCopy, true);
    assertPresentInOutput(output, "most recently active bongoses:", "section header");
    assertPresentInOutput(output, '"_id" : ' + tojson(s1Host), "recent bongos hostname");
    assertNotPresentInOutput(output, '"_id" : ' + tojson(s2Host), "old bongos hostname");

    // Older bongoses
    configCopy.bongos.remove({_id: s1Host});

    var output = grabStatusOutput(configCopy, false);
    assertPresentInOutput(output, "most recently active bongoses:", "section header");
    assertPresentInOutput(output, tojson(version) + " : 1\n", "recent bongos version");

    var output = grabStatusOutput(configCopy, true);
    assertPresentInOutput(output, "most recently active bongoses:", "section header");
    assertNotPresentInOutput(output, '"_id" : ' + tojson(s1Host), "removed bongos hostname");
    assertPresentInOutput(output, '"_id" : ' + tojson(s2Host), "recent bongos hostname");

    // No bongoses at all
    configCopy.bongos.remove({});

    var output = grabStatusOutput(configCopy, false);
    assertPresentInOutput(output, "most recently active bongoses:\n\tnone", "no bongoses");

    var output = grabStatusOutput(configCopy, true);
    assertPresentInOutput(
        output, "most recently active bongoses:\n\tnone", "no bongoses (verbose)");

    assert(bongos.getDB(dbName).dropDatabase());

    ////////////////////////
    // Extended tests
    ////////////////////////

    var testCollDetailsNum = 0;
    function testCollDetails(args) {
        if (args === undefined || typeof(args) != "object") {
            args = {};
        }

        var getCollName = function(x) {
            return "test.test" + x.zeroPad(4);
        };
        var collName = getCollName(testCollDetailsNum);

        var cmdObj = {shardCollection: collName, key: {_id: 1}};
        if (args.unique) {
            cmdObj.unique = true;
        }
        assert.commandWorked(admin.runCommand(cmdObj));

        if (args.hasOwnProperty("unique")) {
            assert.writeOK(bongos.getDB("config").collections.update(
                {_id: collName}, {$set: {"unique": args.unique}}));
        }
        if (args.hasOwnProperty("noBalance")) {
            assert.writeOK(bongos.getDB("config").collections.update(
                {_id: collName}, {$set: {"noBalance": args.noBalance}}));
        }

        var output = grabStatusOutput(st.config);

        assertPresentInOutput(output, collName, "collection");
        // If any of the previous collection names are present, then their optional indicators
        // might also be present.  This might taint the results when we go searching through
        // the output.
        // This also means that earlier collNames can't be a prefix of later collNames.
        for (var i = 0; i < testCollDetailsNum; i++) {
            assertNotPresentInOutput(output, getCollName(i), "previous collection");
        }

        assertPresentInOutput(output, "unique: " + (!!args.unique), "unique shard key indicator");
        if (args.hasOwnProperty("unique") && typeof(args.unique) != "boolean") {
            // non-bool: actual value must be shown
            assertPresentInOutput(
                output, tojson(args.unique), "unique shard key indicator (non bool)");
        }

        assertPresentInOutput(output,
                              "balancing: " + (!args.noBalance),
                              "balancing indicator (inverse of noBalance)");
        if (args.hasOwnProperty("noBalance") && typeof(args.noBalance) != "boolean") {
            // non-bool: actual value must be shown
            assertPresentInOutput(output, tojson(args.noBalance), "noBalance indicator (non bool)");
        }

        assert(bongos.getCollection(collName).drop());

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

    assert(bongos.getDB("test").dropDatabase());

    st.stop();
})();
