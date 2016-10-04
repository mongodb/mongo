/**
 * Test the upgrade/downgrade process for the last stable release <~~> the latest release with both
 * the mmapv1 and wiredTiger storage engines. Repeat the process with --directoryperdb set.
 */
(function() {
    "use strict";

    jsTestLog("Setting up initial data set with the last stable version of mongod");

    var toolTest = new ToolTest('transitioning_to_and_from_WT', {
        binVersion: MongoRunner.getBinVersionFor("last-stable"),
        storageEngine: "mmapv1",
    });

    toolTest.dbpath = toolTest.root + "/original/";
    resetDbpath(toolTest.dbpath);
    assert(mkdir(toolTest.dbpath));
    toolTest.startDB('foo');

    // where we'll put the dump
    var dumpTarget = toolTest.root + '/transitioning_to_and_from_WT_dump/';

    // the db and collections we'll be using
    var testDB = toolTest.db.getSiblingDB('test');
    var longName = "this_name_is_just_63_characters_because_that_is_the_name_limit";
    var testColl = testDB.coll;
    testDB.createCollection("capped", {capped: true, size: 10000});
    var testCapped = testDB.capped;
    // test database and collection lengths to make sure they work correctly in latest and with WT
    var longDB = toolTest.db.getSiblingDB(longName);
    var longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;
    longColl.insert({x: 1});

    // insert some documents
    for (var i = 0; i < 50; i++) {
        if (i < 10) {
            testCapped.insert({x: i});
        }
        testColl.insert({x: i});
    }
    // create an index
    testColl.ensureIndex({x: 1}, {name: "namedIndex"});

    // sanity check the insert worked
    var indexes = testColl.getIndexes();
    assert.eq(50, testColl.count());
    assert.eq(2, indexes.length);
    assert(indexes[0].name === "namedIndex" || indexes[1].name === "namedIndex");
    assert.eq(10, testCapped.count());
    assert(testCapped.isCapped());
    assert.eq(1, longColl.count());

    // Transition from the last stable version with mmapv1...
    var modes = [
        // to the latest version with wiredTiger
        {
          binVersion: "latest",
          storageEngine: "wiredTiger",
        },
        // back to the last stable version with mmapv1
        {
          binVersion: "last-stable",
          storageEngine: "mmapv1",
        },
        // to the latest version with mmapv1
        {
          binVersion: "latest",
          storageEngine: "mmapv1",
        },
        // to latest version with wiredTiger
        {
          binVersion: "latest",
          storageEngine: "wiredTiger",
        },
        // back to the latest version with mmapv1
        {
          binVersion: "latest",
          storageEngine: "mmapv1",
        },
        // to the last stable version with mmapv1 and directory per db
        {
          binVersion: "last-stable",
          storageEngine: "mmapv1",
          directoryperdb: "",
        },
        // to the latest version with wiredTiger
        {
          binVersion: "latest",
          storageEngine: "wiredTiger",
        },
        // back to the last stable version with mmapv1 and directory per db
        {
          binVersion: "last-stable",
          storageEngine: "mmapv1",
          directoryperdb: "",
        },
        // to latest version with mmapv1 and directory per db
        {
          binVersion: "latest",
          storageEngine: "mmapv1",
          directoryperdb: "",
        },
        // to the latest with wiredTiger
        {
          binVersion: "latest",
          storageEngine: "wiredTiger",
        },
        // back to latest version with mmapv1 and directory per db
        {
          binVersion: "latest",
          storageEngine: "mmapv1",
          directoryperdb: "",
        },
    ];

    modes.forEach(function(entry, idx) {
        jsTestLog("moving to: " + tojson(entry));
        // dump the data
        resetDbpath(dumpTarget);
        var ret = toolTest.runTool('dump', '--out', dumpTarget);
        assert.eq(0, ret);

        // stop previous mongod
        MongoRunner.stopMongod(toolTest.port);

        // clear old node configuration info
        toolTest.m = null;
        toolTest.db = null;

        // set up new node configuration info
        toolTest.options.binVersion = MongoRunner.getBinVersionFor(entry.binVersion);
        toolTest.dbpath =
            toolTest.root + "/" + idx + "-" + entry.binVersion + "-" + entry.storageEngine + "/";

        if (entry.hasOwnProperty("storageEngine")) {
            toolTest.options.storageEngine = entry.storageEngine;
        }

        if (entry.hasOwnProperty("directoryperdb")) {
            toolTest.options.directoryperdb = entry.directoryperdb;
        }

        // create the unique dbpath for this instance and start the mongod
        resetDbpath(toolTest.dbpath);
        assert(mkdir(toolTest.dbpath));
        toolTest.startDB('foo');

        // refresh the db and coll reference
        testDB = toolTest.db.getSiblingDB('test');
        testCapped = testDB.capped;
        testColl = testDB.coll;
        longDB = toolTest.db.getSiblingDB(longName);
        longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

        // ensure the new mongod was started with an empty data dir
        assert.eq(0, testColl.count());
        assert.eq(0, testCapped.count());
        assert.eq(0, longColl.count());

        // restore the data
        ret = toolTest.runTool('restore', dumpTarget);
        assert.eq(0, ret);

        // make sure the data was restored
        assert.eq(50, testColl.count());
        indexes = testColl.getIndexes();
        assert.eq(2, indexes.length);
        assert(indexes[0].name === "namedIndex" || indexes[1].name === "namedIndex");
        assert.eq(10, testCapped.count());
        assert(testCapped.isCapped());
        for (var i = 0; i < 50; i++) {
            if (i < 10) {
                assert.eq(1, testCapped.count({x: i}));
            }
            assert.eq(1, testColl.count({x: i}));
        }
        assert.eq(1, longColl.count());
    });

    // success
    toolTest.stop();
}());
