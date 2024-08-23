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

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ResumableIndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getDB(dbName).getCollection(collName);

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

rst.stopSet();
