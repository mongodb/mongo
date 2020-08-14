/**
 * Tests that if an index build fails to resume during setup (before the index builds thread is
 * created), the index build will successfully restart from the beginning.
 *
 * @tags: [requires_persistence, requires_replication]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const collName = jsTestName();

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let coll = primary.getDB(dbName).getCollection(collName);

if (!ResumableIndexBuildTest.resumableIndexBuildsEnabled(primary)) {
    jsTestLog("Skipping test because resumable index builds are not enabled");
    rst.stopSet();
    return;
}

assert.commandWorked(coll.insert({a: 1}));

ResumableIndexBuildTest.runFailToResume(rst,
                                        dbName,
                                        collName,
                                        {a: 1},
                                        "failToParseResumeIndexInfo",
                                        [{a: 2}, {a: 3}],
                                        [{a: 4}, {a: 5}],
                                        true /* failWhileParsing */);
assert.commandWorked(
    primary.adminCommand({configureFailPoint: 'failToParseResumeIndexInfo', mode: 'off'}));

ResumableIndexBuildTest.runFailToResume(
    rst, dbName, collName, {a: 1}, "failSetUpResumeIndexBuild", [{a: 6}, {a: 7}], [{a: 8}, {a: 9}]);

rst.stopSet();
})();