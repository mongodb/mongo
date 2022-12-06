/**
 * Tests support for _refreshQueryAnalyzerConfiguration command on a non-sharded cluster. Verifies
 * that it is not supported on any mongod.
 *
 * @tags: [requires_fcv_63, featureFlagAnalyzeShardKey]
 */
(function() {
"use strict";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const sampleRate = 1;

assert.commandWorked(primary.getDB(dbName).createCollection(collName));
assert.commandWorked(primary.adminCommand({configureQueryAnalyzer: ns, mode: "full", sampleRate}));

assert.commandFailedWithCode(primary.adminCommand({
    _refreshQueryAnalyzerConfiguration: 1,
    name: primary.host,
    numQueriesExecutedPerSecond: 1
}),
                             ErrorCodes.IllegalOperation);

assert.commandFailedWithCode(secondary.adminCommand({
    _refreshQueryAnalyzerConfiguration: 1,
    name: secondary.host,
    numQueriesExecutedPerSecond: 1
}),
                             ErrorCodes.NotWritablePrimary);
rst.stopSet();
})();
