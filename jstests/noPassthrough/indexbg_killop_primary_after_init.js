/**
 * Confirms that background index builds on a primary can be aborted using killop
 * on the client connection operation when the IndexBuildsCoordinator is enabled.
 * @tags: [requires_replication]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/noPassthrough/libs/index_build.js');

const rst = new ReplSetTest({
    nodes: [
        {},
    ],
});
const nodes = rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDB = primary.getDB('test');
const coll = testDB.getCollection('test');

assert.commandWorked(coll.insert({a: 1}));

const res = assert.commandWorked(primary.adminCommand(
    {configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'alwaysOn'}));
const failpointTimesEntered = res.count;

const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

try {
    assert.commandWorked(primary.adminCommand({
        waitForFailPoint: "hangAfterInitializingIndexBuild",
        timesEntered: failpointTimesEntered + 1,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // When the index build starts, find its op id. This will be the op id of the client
    // connection, not the thread pool task managed by IndexBuildsCoordinatorMongod.
    const filter = {"desc": {$regex: /conn.*/}};
    const opId = IndexBuildTest.waitForIndexBuildToStart(testDB, coll.getName(), 'a_1', filter);

    // Index build should be present in the config.system.indexBuilds collection.
    const indexMap =
        IndexBuildTest.assertIndexesSoon(coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
    const indexBuildUUID = indexMap['a_1'].buildUUID;
    assert(primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

    // Kill the index build and wait for it to abort.
    assert.commandWorked(testDB.killOp(opId));
    checkLog.containsJson(primary, 4656003);
} finally {
    assert.commandWorked(
        primary.adminCommand({configureFailPoint: 'hangAfterInitializingIndexBuild', mode: 'off'}));
}

// Wait for the index build to stop.
IndexBuildTest.waitForIndexBuildToStop(testDB);

const exitCode = createIdx({checkExitSuccess: false});
assert.neq(0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

checkLog.containsJson(primary, 20443);

// Check that no new index has been created.  This verifies that the index build was aborted
// rather than successfully completed.
IndexBuildTest.assertIndexes(coll, 1, ['_id_']);

// Two-phase index builds replicate different oplog entries.
const cmdNs = testDB.getCollection('$cmd').getFullName();
let ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.startIndexBuild': coll.getName()});
assert.eq(1, ops.length, 'incorrect number of startIndexBuild oplog entries: ' + tojson(ops));
ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': coll.getName()});
assert.eq(1, ops.length, 'incorrect number of abortIndexBuild oplog entries: ' + tojson(ops));
const indexBuildUUID = ops[0].o.indexBuildUUID;
ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.commitIndexBuild': coll.getName()});
assert.eq(0, ops.length, 'incorrect number of commitIndexBuild oplog entries: ' + tojson(ops));

// Index build should be removed from the config.system.indexBuilds collection.
assert.isnull(primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

rst.stopSet();
})();
