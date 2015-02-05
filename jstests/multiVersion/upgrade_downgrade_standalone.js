// Test drop in binary replacement upgrade/downgrade process for:
//      2.6 -> 2.8
//      2.8 -> 2.6
//      2.6 directoryperdb -> 2.8 directoryperdb
//      2.8 directoryperdb -> 2.6 directoryperdb

(function() {

    "use strict";
    var oldVersion = "2.6"
    var newVersion = "2.8"

    var runUpOrDownGrade = function(fromVersion, toVersion, dirPerDb) {
        jsTestLog("switching from " + fromVersion + " to " + toVersion +
                (dirPerDb !== null ? " " : " not ") + "using directoryperdb");
        jsTestLog("setting up initial data for a " + fromVersion);

        var conn = MongoRunner.runMongod({binVersion: fromVersion,
                                          cleanData: true,
                                          directoryperdb: dirPerDb});

        // the db and collections we'll be using
        var testDB = conn.getDB('test');
        var longName = "this_name_is_just_63_characters_because_that_is_the_name_limit";
        var testColl = testDB.coll;
        testDB.createCollection("capped", {capped: true, size: 10000});
        var testCapped = testDB.capped;
        // test database and collection lengths to make sure they work correctly in 2.8 and with WT
        var longDB = conn.getDB(longName);
        var longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

        // insert some documents
        longColl.insert({x: 1});
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

        jsTestLog("upgrading/downgrading to " + toVersion);

        // stop previous mongod
        MongoRunner.stopMongod(conn);

        // start the other mongod
        conn = MongoRunner.runMongod({binVersion: toVersion,
                                      noCleanData: true,
                                      directoryperdb: dirPerDb});

        // refresh the db and coll reference
        testDB = conn.getDB('test');
        testColl = testDB.coll;
        testCapped = testDB.capped;
        // test database and collection lengths to make sure they work correctly in 2.8 and with WT
        longDB = conn.getDB(longName);
        longColl = longDB.collection_name_is_lengthed_to_reach_namespace_max_of_123;

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
        MongoRunner.stopMongod(conn);
    }

    runUpOrDownGrade(oldVersion, newVersion);
    runUpOrDownGrade(newVersion, oldVersion);
    runUpOrDownGrade(oldVersion, newVersion, "");
    runUpOrDownGrade(newVersion, oldVersion, "");

}());
