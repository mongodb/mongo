// Tests user queries over the config servers.
(function() {
    'use strict';

    var getListCollectionsCursor = function(database, options, subsequentBatchSize) {
        return new DBCommandCursor(database.getMongo(),
                                   database.runCommand("listCollections", options),
                                   subsequentBatchSize);
    };

    var getListIndexesCursor = function(coll, options, subsequentBatchSize) {
        return new DBCommandCursor(
            coll.getDB().getMongo(), coll.runCommand("listIndexes", options), subsequentBatchSize);
    };

    var arrayGetNames = function(array) {
        return array.map(function(spec) {
            return spec.name;
        });
    };

    var cursorGetCollectionNames = function(cursor) {
        return arrayGetNames(cursor.toArray());
    };

    var sortArrayByName = function(array) {
        return array.sort(function(a, b) {
            return a.name > b.name;
        });
    };

    var cursorGetIndexNames = function(cursor) {
        return arrayGetNames(sortArrayByName(cursor.toArray()));
    };

    var sortArrayById = function(array) {
        return array.sort(function(a, b) {
            return a._id > b._id;
        });
    };

    var dropCollectionIfExists = function(coll) {
        try {
            coll.drop();
        } catch (err) {
            assert.eq(err.code, ErrorCodes.NamespaceNotFound);
        }
    };

    /**
     * Sets up the test database with with several sharded collections.
     *
     * @return The list of collection namespaces that were added to the test database.
     */
    var setupTestCollections = function(st) {
        // testKeys and testCollNames are parallel arrays, testKeys contains the shard key of the
        // corresponding collection whose name is in testCollNames.
        var testCollNames = ["4a1", "1a12", "3a1b1", "2a1b1c1", "b1", "b1c1", "d1"];
        var testKeys =
            [{a: 1}, {a: 1}, {a: 1, b: 1}, {a: 1, b: 1, c: 1}, {b: 1}, {b: 1, c: 1}, {d: 1}];
        var testDB = st.s.getDB("test");

        assert.commandWorked(st.s.adminCommand({enablesharding: testDB.getName()}));
        var testNamespaces = testCollNames.map(function(e) {
            return testDB.getName() + "." + e;
        });
        for (var i = 0; i < testKeys.length; i++) {
            assert.commandWorked(
                st.s.adminCommand({shardcollection: testNamespaces[i], key: testKeys[i]}));
        }

        return testNamespaces;
    };

    /**
     * Test that a list collections query works on the config database. This test cannot detect
     * whether list collections lists extra collections.
     */
    var testListConfigCollections = function(st) {
        // This test depends on all the collections in the configCollList being in the config
        // database.
        var configCollList = [
            "changelog",
            "chunks",
            "collections",
            "databases",
            "lockpings",
            "locks",
            "mongos",
            "settings",
            "shards",
            "tags",
            "version"
        ];
        var configDB = st.s.getDB("config");
        var userAddedColl = configDB.userAddedColl;
        var cursor;
        var cursorArray;

        dropCollectionIfExists(userAddedColl);
        configDB.createCollection(userAddedColl.getName());
        configCollList.push(userAddedColl.getName());

        cursor = getListCollectionsCursor(configDB);
        cursorArray = cursorGetCollectionNames(cursor);
        for (var i = 0; i < configCollList.length; i++) {
            assert(cursorArray.indexOf(configCollList[i]) > -1, "Missing " + configCollList[i]);
        }

        cursor = getListCollectionsCursor(configDB, {cursor: {batchSize: 1}}, 1);
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert(cursorArray.indexOf(cursor.next().name) > -1);
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert(cursorArray.indexOf(cursor.next().name) > -1);

        assert(userAddedColl.drop());
    };

    /**
     * Test that a list indexes query works on the chunks collection of the config database.
     */
    var testListConfigChunksIndexes = function(st) {
        // This test depends on all the indexes in the configChunksIndexes being the exact indexes
        // in the config chunks collection.
        var configChunksIndexes = ["_id_", "ns_1_lastmod_1", "ns_1_min_1", "ns_1_shard_1_min_1"];
        var configDB = st.s.getDB("config");
        var cursor;
        var cursorArray = [];

        cursor = getListIndexesCursor(configDB.chunks);
        assert.eq(cursorGetIndexNames(cursor), configChunksIndexes);

        cursor = getListIndexesCursor(configDB.chunks, {cursor: {batchSize: 2}}, 2);
        assert.eq(cursor.objsLeftInBatch(), 2);
        cursorArray.push(cursor.next());
        cursorArray.push(cursor.next());
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 2);
        cursorArray.push(cursor.next());
        cursorArray.push(cursor.next());
        assert(!cursor.hasNext());
        assert.eq(arrayGetNames(sortArrayByName(cursorArray)), configChunksIndexes);
    };

    /**
     * Test queries over the collections collection of the config database.
     */
    var queryConfigCollections = function(st, testNamespaces) {
        var configDB = st.s.getDB("config");
        var cursor;

        // Find query.
        cursor = configDB.collections.find({"key.a": 1}, {dropped: 1, "key.a": 1, "key.c": 1})
                     .sort({"_id": 1})
                     .batchSize(2);
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {_id: testNamespaces[1], dropped: false, key: {a: 1}});
        assert.eq(cursor.next(), {_id: testNamespaces[3], dropped: false, key: {a: 1, c: 1}});
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {_id: testNamespaces[2], dropped: false, key: {a: 1}});
        assert.eq(cursor.next(), {_id: testNamespaces[0], dropped: false, key: {a: 1}});
        assert(!cursor.hasNext());

        // Aggregate query.
        cursor = configDB.collections.aggregate(
            [
              {$match: {"key.b": 1}},
              {$sort: {"_id": 1}},
              {$project: {"keyb": "$key.b", "keyc": "$key.c"}}
            ],
            {cursor: {batchSize: 2}});
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {_id: testNamespaces[3], keyb: 1, keyc: 1});
        assert.eq(cursor.next(), {_id: testNamespaces[2], keyb: 1});
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {_id: testNamespaces[4], keyb: 1});
        assert.eq(cursor.next(), {_id: testNamespaces[5], keyb: 1, keyc: 1});
        assert(!cursor.hasNext());

        // Count query without filter.
        assert.eq(configDB.collections.count(), testNamespaces.length);
    };

    /**
     * Test queries over the chunks collection of the config database.
     */
    var queryConfigChunks = function(st) {
        var configDB = st.s.getDB("config");
        var testDB = st.s.getDB("test2");
        var testColl = testDB.testColl;
        var testCollData = [{e: 1}, {e: 3}, {e: 4}, {e: 5}, {e: 7}, {e: 9}, {e: 10}, {e: 12}];
        var cursor;
        var result;

        // Get shard names.
        cursor = configDB.shards.find().sort({_id: 1});
        var shard1 = cursor.next()._id;
        var shard2 = cursor.next()._id;
        assert(!cursor.hasNext());
        assert.commandWorked(st.s.adminCommand({enablesharding: testDB.getName()}));
        st.ensurePrimaryShard(testDB.getName(), shard1);

        // Setup.
        assert.commandWorked(
            st.s.adminCommand({shardcollection: testColl.getFullName(), key: {e: 1}}));
        for (var i = 0; i < testCollData.length; i++) {
            assert.writeOK(testColl.insert(testCollData[i]));
        }
        assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 2}}));
        assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 6}}));
        assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 8}}));
        assert.commandWorked(st.s.adminCommand({split: testColl.getFullName(), middle: {e: 11}}));
        assert.commandWorked(
            st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 1}, to: shard2}));
        assert.commandWorked(
            st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 9}, to: shard2}));
        assert.commandWorked(
            st.s.adminCommand({movechunk: testColl.getFullName(), find: {e: 12}, to: shard2}));

        // Find query.
        cursor =
            configDB.chunks.find({ns: testColl.getFullName()}, {_id: 0, min: 1, max: 1, shard: 1})
                .sort({"min.e": 1});
        assert.eq(cursor.next(), {min: {e: {"$minKey": 1}}, "max": {"e": 2}, shard: shard2});
        assert.eq(cursor.next(), {min: {e: 2}, max: {e: 6}, shard: shard1});
        assert.eq(cursor.next(), {min: {e: 6}, max: {e: 8}, shard: shard1});
        assert.eq(cursor.next(), {min: {e: 8}, max: {e: 11}, shard: shard2});
        assert.eq(cursor.next(), {min: {e: 11}, max: {e: {"$maxKey": 1}}, shard: shard2});
        assert(!cursor.hasNext());

        // Count query with filter.
        assert.eq(configDB.chunks.count({ns: testColl.getFullName()}), 5);

        // Distinct query.
        assert.eq(configDB.chunks.distinct("shard").sort(), [shard1, shard2]);

        // Group query.
        result = configDB.chunks.group({
            key: {shard: 1},
            cond: {ns: testColl.getFullName()},
            reduce: function(curr, res) {
                res.chunks++;
            },
            initial: {chunks: 0},
            finalize: function(res) {
                res._id = res.shard;
            }
        });
        assert.eq(
            sortArrayById(result),
            [{shard: shard1, chunks: 2, _id: shard1}, {shard: shard2, chunks: 3, _id: shard2}]);

        // Map reduce query.
        var mapFunction = function() {
            if (this.ns == "test2.testColl") {
                emit(this.shard, 1);
            }
        };
        var reduceFunction = function(key, values) {
            return {chunks: values.length};
        };
        result = configDB.chunks.mapReduce(mapFunction, reduceFunction, {out: {inline: 1}});
        assert.eq(result.ok, 1);
        assert.eq(sortArrayById(result.results),
                  [{_id: shard1, value: {chunks: 2}}, {_id: shard2, value: {chunks: 3}}]);
    };

    /**
     * Test queries over a user created collection of an arbitrary database on the config servers.
     */
    var queryUserCreated = function(database) {
        var userColl = database.userColl;
        var userCollData = [
            {_id: 1, g: 1, c: 4, s: "c", u: [1, 2]},
            {_id: 2, g: 1, c: 5, s: "b", u: [1]},
            {_id: 3, g: 2, c: 16, s: "g", u: [3]},
            {_id: 4, g: 2, c: 1, s: "a", u: [2, 4]},
            {_id: 5, g: 2, c: 18, s: "d", u: [3]},
            {_id: 6, g: 3, c: 11, s: "e", u: [2, 3]},
            {_id: 7, g: 3, c: 2, s: "f", u: [1]}
        ];
        var userCollIndexes = ["_id_", "s_1"];
        var cursor;
        var cursorArray;
        var result;

        // Setup.
        dropCollectionIfExists(userColl);
        for (var i = 0; i < userCollData.length; i++) {
            assert.writeOK(userColl.insert(userCollData[i]));
        }
        assert.commandWorked(userColl.createIndex({s: 1}));

        // List indexes.
        cursorArray = [];
        cursor = getListIndexesCursor(userColl, {cursor: {batchSize: 1}}, 1);
        assert.eq(cursor.objsLeftInBatch(), 1);
        cursorArray.push(cursor.next());
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 1);
        cursorArray.push(cursor.next());
        assert(!cursor.hasNext());
        assert.eq(arrayGetNames(sortArrayByName(cursorArray)), userCollIndexes);

        // Find query.
        cursor = userColl.find({g: {$gte: 2}}, {_id: 0, c: 1}).sort({s: 1}).batchSize(2);
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {c: 1});
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert.eq(cursor.next(), {c: 18});
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {c: 11});
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert.eq(cursor.next(), {c: 2});
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert.eq(cursor.next(), {c: 16});
        assert(!cursor.hasNext());

        // Aggregate query.
        cursor = userColl.aggregate(
            [
              {$match: {c: {$gt: 1}}},
              {$unwind: "$u"},
              {$group: {_id: "$u", sum: {$sum: "$c"}}},
              {$sort: {_id: 1}}
            ],
            {cursor: {batchSize: 2}});
        assert.eq(cursor.objsLeftInBatch(), 2);
        assert.eq(cursor.next(), {_id: 1, sum: 11});
        assert.eq(cursor.next(), {_id: 2, sum: 15});
        assert(cursor.hasNext());
        assert.eq(cursor.objsLeftInBatch(), 1);
        assert.eq(cursor.next(), {_id: 3, sum: 45});
        assert(!cursor.hasNext());

        // Count query without filter.
        assert.eq(userColl.count(), userCollData.length);

        // Count query with filter.
        assert.eq(userColl.count({g: 2}), 3);

        // Distinct query.
        assert.eq(userColl.distinct("g").sort(), [1, 2, 3]);

        // Group query.
        result = userColl.group({
            key: {g: 1},
            reduce: function(curr, res) {
                res.prod *= curr.c;
            },
            initial: {prod: 1},
            finalize: function(res) {
                res._id = res.g;
            }
        });
        assert.eq(sortArrayById(result),
                  [{g: 1, prod: 20, _id: 1}, {g: 2, prod: 288, _id: 2}, {g: 3, prod: 22, _id: 3}]);

        // Map reduce query.
        var mapFunction = function() {
            emit(this.g, 1);
        };
        var reduceFunction = function(key, values) {
            return {count: values.length};
        };
        result = userColl.mapReduce(mapFunction, reduceFunction, {out: {inline: 1}});
        assert.eq(result.ok, 1);
        assert.eq(sortArrayById(result.results), [
            {_id: 1, value: {count: 2}},
            {_id: 2, value: {count: 3}},
            {_id: 3, value: {count: 2}}
        ]);

        assert(userColl.drop());
    };

    var st = new ShardingTest({shards: 2, mongos: 1});
    var testNamespaces = setupTestCollections(st);
    var configDB = st.s.getDB("config");
    var adminDB = st.s.getDB("admin");

    testListConfigCollections(st);
    testListConfigChunksIndexes(st);
    queryConfigCollections(st, testNamespaces);
    queryConfigChunks(st);
    queryUserCreated(configDB);
    queryUserCreated(adminDB);
})();
