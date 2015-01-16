// Test dump/restore upgrade/downgrade/transition process for:
//      2.6 -> 2.8 wiredTiger
//      2.8 wiredTiger -> 2.6
//      2.6 -> 2.8
//      2.8 -> 2.8 wiredTiger
//      2.8 wiredTiger -> 2.8
// then back to 2.6 and rerun each transition with directoryperdb on all non-wiredTiger instances

(function() {

    "use strict";

    jsTestLog('Setting up initial data set with a 2.6 mongod');

    var toolTest = new ToolTest('transitioning_to_and_from_WT', { binVersion: '2.6' });
    toolTest.dbpath = toolTest.root + "/original26/";
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
    // test database and collection lengths to make sure they work correctly in 2.8 and with WT
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

    // modes contains the descriptions of the assorted mongod configurations will we dump/restore
    // between in the order they will be used

    // version is either "2.6" or null which indicates latest
    // storageEngine is either "wiredTiger" or null which indicates mmapv1
    // directoryperdb is either "" which indicates it will be used or null which indicates it won't
    var modes = [
        // to 2.8 wired tiger
        {
            version: null,
            storageEngine: "wiredTiger",
            directoryperdb: null,
        },
        // back to 2.6
        {
            version: "2.6",
            storageEngine: null,
            directoryperdb: null,
        },
        // to 2.8 mmapv1
        {
            version: null,
            storageEngine: null,
            directoryperdb: null,
        },
        // to 2.8 wired tiger
        {
            version: null,
            storageEngine: "wiredTiger",
            directoryperdb: null,
        },
        // back to 2.8 mmapv1
        {
            version: null,
            storageEngine: null,
            directoryperdb: null,
        },
        // to 2.6 dir per db
        {
            version: "2.6",
            storageEngine: null,
            directoryperdb: "",
        },
        // to 2.8 wired tiger
        {
            version: null,
            storageEngine: "wiredTiger",
            directoryperdb: null,
        },
        // back to 2.6 dir per db
        {
            version: "2.6",
            storageEngine: null,
            directoryperdb: "",
        },
        // to 2.8 mmapv1 dir per db
        {
            version: null,
            storageEngine: null,
            directoryperdb: "",
        },
        // to 2.8 wired tiger
        {
            version: null,
            storageEngine: "wiredTiger",
            directoryperdb: null,
        },
        // back to 2.8 mmapv1 dir per db
        {
            version: null,
            storageEngine: null,
            directoryperdb: "",
        },
    ];

    for (var idx = 0; idx < modes.length; idx++) {
        var entry = modes[idx];

        jsTestLog("moving to: " + tojson(entry));
        // dump the data 
        resetDbpath(dumpTarget);
        var ret = toolTest.runTool('dump', '--out', dumpTarget);
        assert.eq(0, ret);

        // stop previous mongod
        stopMongod(toolTest.port);

        // clear old node configuration info
        toolTest.m = null;
        toolTest.db = null;
        /// set up new node configuration info
        toolTest.options.binVersion = entry.version;
        toolTest.dbpath = toolTest.root + "/" + idx + entry.version + entry.storageEngine + "/";
        toolTest.options.storageEngine = entry.storageEngine;
        toolTest.options.directoryperdb = entry.directoryperdb;

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
    }

    // success
    toolTest.stop();

}());
