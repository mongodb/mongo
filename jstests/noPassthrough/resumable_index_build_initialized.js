/**
 * Tests that resumable index build state is written to disk upon clean shutdown when an index
 * build has been initialized but has not yet begun the collection scan phase.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */
(function() {
"use strict";

load("jstests/noPassthrough/libs/index_build.js");

const dbName = "test";
const failPointName = "hangIndexBuildBeforeWaitingUntilMajorityOpTime";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const coll = rst.getPrimary().getDB(dbName).getCollection(jsTestName());
assert.commandWorked(coll.insert({a: 1}));

ResumableIndexBuildTest.run(rst, dbName, coll.getName(), {a: 1}, failPointName, {});

rst.stopSet();
})();