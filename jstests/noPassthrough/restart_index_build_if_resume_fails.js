/**
 * Tests that if an index build fails to resume during setup (before the index builds thread is
 * created), the index build will successfully restart from the beginning.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   uses_column_store_index,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");
load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getDB(dbName).getCollection(collName);
const columnstoreEnabled = checkSBEEnabled(primary.getDB(dbName),
                                           ["featureFlagColumnstoreIndexes", "featureFlagSbeFull"],
                                           true /* checkAllNodes */);

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

// TODO (SERVER-65978): Add side writes to these test cases once they are supported by column store
// index builds.
if (columnstoreEnabled) {
    ResumableIndexBuildTest.runFailToResume(rst,
                                            dbName,
                                            collName,
                                            {"$**": "columnstore"},
                                            {failPointAfterStartup: "failToParseResumeIndexInfo"},
                                            [],
                                            [{a: 4}, {a: 5}],
                                            true /* failWhileParsing */);

    ResumableIndexBuildTest.runFailToResume(rst,
                                            dbName,
                                            collName,
                                            {"$**": "columnstore"},
                                            {failPointAfterStartup: "failSetUpResumeIndexBuild"},
                                            [],
                                            [{a: 8}, {a: 9}]);

    ResumableIndexBuildTest.runFailToResume(rst,
                                            dbName,
                                            collName,
                                            {"$**": "columnstore"},
                                            {removeTempFilesBeforeStartup: true},
                                            [],
                                            [{a: 12}, {a: 13}]);
}

rst.stopSet();
})();
