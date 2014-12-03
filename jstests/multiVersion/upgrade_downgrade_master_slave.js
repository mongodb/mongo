// Test drop in binary replacement upgrade/downgrade process for master/slave cases:
//      2.6 -> 2.8
//      2.8 -> 2.6
//      2.6 directoryperdb -> 2.8 directoryperdb
//      2.8 directoryperdb -> 2.6 directoryperdb

(function() {

    "use strict";
    var oldVersion = "2.6";
    var newVersion = "2.8";

    var runUpOrDownGrade = function(fromVersion, toVersion, dirPerDb) {
        jsTestLog("switching from " + fromVersion + " to " + toVersion +
                (dirPerDb !== null ? " " : " not ") + "using directoryperdb");
        jsTestLog("setting up initial data for a " + fromVersion);

        var replTest = new ReplTest("name");
        var conn = replTest.start(true,
                                  {binVersion: fromVersion,
                                   cleanData: true,
                                   directoryperdb: dirPerDb});
        conn.forceWriteMode("commands");
        var slave = replTest.start(false,
                                   {binVersion: fromVersion,
                                    cleanData: true,
                                    directoryperdb: dirPerDb});

        // the db and collections we'll be using
        var writeConcern = {writeConcern: {w: 2, wtimeout: 10*1000}};
        var testDB = conn.getDB('test');
        var longName = "this_name_is_just_63_characters_because_that_is_the_name_limit";
        var testColl = testDB.coll;
        testDB.createCollection("capped", {capped: true, size: 10000});
        var testCapped = testDB.capped;
        var longDB = conn.getDB(longName);
        var longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

        // insert some documents
        for (var i = 0; i < 50; i++) {
            if (i < 10) {
                testCapped.insert({x: i}, writeConcern);
            }
            testColl.insert({x: i}, writeConcern);
        }
        // create an index
        testColl.ensureIndex({x: 1}, {name: "namedIndex"}, writeConcern);
        assert.writeOK(longColl.insert({x: 1}, writeConcern));

        // sanity check the insert worked
        var indexes = testColl.getIndexes();
        assert.eq(50, testColl.count());
        assert.eq(2, indexes.length);
        assert(indexes[0].name === "namedIndex" || indexes[1].name === "namedIndex");
        assert.eq(10, testCapped.count());
        assert(testCapped.isCapped());
        assert.eq(1, longColl.count());

        // check on the slave as well
        testDB = slave.getDB('test');
        testColl = testDB.coll;
        indexes = testColl.getIndexes();
        testCapped = testDB.capped;
        longDB = slave.getDB(longName);
        longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

        assert.eq(50, testColl.count());
        assert.eq(2, indexes.length);
        assert(indexes[0].name === "namedIndex" || indexes[1].name === "namedIndex");
        assert.eq(10, testCapped.count());
        assert(testCapped.isCapped());
        assert.eq(1, longColl.count());

        jsTestLog("upgrading/downgrading to " + toVersion);

        // upgrade/downgrade slave
        replTest.stop(false);
        slave = replTest.start(false,
                               {binVersion: toVersion,
                                noCleanData: true,
                                directoryperdb: dirPerDb},
                               true);

        // upgrade/downgrade master
        replTest.stop(true);
        conn = replTest.start(true,
                              {binVersion: toVersion,
                               noCleanData: true,
                               directoryperdb: dirPerDb},
                               true);

        // refresh the db and coll reference
        testDB = conn.getDB('test');
        testColl = testDB.coll;
        testCapped = testDB.capped;
        longDB = conn.getDB(longName);
        longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

        // make sure the data was restored
        assert.eq(50, testColl.count());
        indexes = testColl.getIndexes();
        assert.eq(2, indexes.length);
        assert(indexes[0].name === "namedIndex" || indexes[1].name === "namedIndex");
        assert.eq(10, testCapped.count());
        assert(testCapped.isCapped());
        for (i = 0; i < 50; i++) {
            if (i < 10) {
                assert.eq(1, testCapped.count({x: i}));
            }
            assert.eq(1, testColl.count({x: i})); 
        }
        assert.eq(1, longColl.count());

        // check on the slave as well
        testDB = slave.getDB('test');
        testColl = testDB.coll;
        testCapped = testDB.capped;
        longDB = slave.getDB(longName);
        longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;
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

        // stop the cluster
        replTest.stop(false);
        replTest.stop(true);
    };

    runUpOrDownGrade(newVersion, oldVersion);
    runUpOrDownGrade(oldVersion, newVersion);
    runUpOrDownGrade(oldVersion, newVersion, "");
    runUpOrDownGrade(newVersion, oldVersion, "");
}());
