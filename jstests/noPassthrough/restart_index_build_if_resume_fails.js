/**
 * Tests that if an index build fails to resume during setup (before the index builds thread is
 * created), the index build will successfully restart from the beginning.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/sbe_util.js");          // For checkSBEEnabled.
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getDB(dbName).getCollection(collName);
const columnstoreEnabled = checkSBEEnabled(primary.getDB(dbName),
                                           ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"],
                                           true /* checkAllNodes */) &&
    setUpServerForColumnStoreIndexTest(primary.getDB(dbName));

assert.commandWorked(coll.insert({a: 1}));

ResumableIndexBuildTest.runFailToResume(rst,
                                        dbName,
                                        collName,
                                        {a: 1},
                                        {failPointAfterStartup: "failToParseResumeIndexInfo"},
                                        [{a: 2}, {a: 3}],
                                        [{a: 4}, {a: 5}],
                                        true /* failWhileParsing */);

ResumableIndexBuildTest.runFailToResume(rst,
                                        dbName,
                                        collName,
                                        {a: 1},
                                        {failPointAfterStartup: "failSetUpResumeIndexBuild"},
                                        [{a: 6}, {a: 7}],
                                        [{a: 8}, {a: 9}]);

ResumableIndexBuildTest.runFailToResume(rst,
                                        dbName,
                                        collName,
                                        {a: 1},
                                        {removeTempFilesBeforeStartup: true},
                                        [{a: 10}, {a: 11}],
                                        [{a: 12}, {a: 13}]);

if (columnstoreEnabled) {
    ResumableIndexBuildTest.runFailToResume(
        rst,
        dbName,
        collName,
        {"$**": "columnstore"},
        {failPointAfterStartup: "failToParseResumeIndexInfo"},
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 14}]}, {a: 15}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 14}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 15}, {a: 1}));
        }),
        [{a: 16}, {a: 17}],
        true /* failWhileParsing */);

    ResumableIndexBuildTest.runFailToResume(
        rst,
        dbName,
        collName,
        {"$**": "columnstore"},
        {failPointAfterStartup: "failSetUpResumeIndexBuild"},
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 18}]}, {a: 19}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 18}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 19}, {a: 1}));
        }),
        [{a: 20}, {a: 21}]);

    ResumableIndexBuildTest.runFailToResume(
        rst,
        dbName,
        collName,
        {"$**": "columnstore"},
        {removeTempFilesBeforeStartup: true},
        (function(collection) {
            assert.commandWorked(collection.insert([{a: [{b: 22}]}, {a: 23}]));
            assert.commandWorked(collection.update({a: 1}, {a: 2}));
            assert.commandWorked(collection.remove({"a.b": 22}));
            assert.commandWorked(collection.insert({a: 1}));
            assert.commandWorked(collection.remove({a: 1}));
            assert.commandWorked(collection.update({a: 23}, {a: 1}));
        }),
        [{a: 24}, {a: 25}]);
}

rst.stopSet();
})();
