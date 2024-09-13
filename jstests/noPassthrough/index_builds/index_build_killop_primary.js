/**
 * Confirms that background index builds on a primary can be aborted using killop.
 * @tags: [
 *   requires_replication,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

function killopOnFailpoint(rst, failpointName, collName) {
    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection(collName);

    assert.commandWorked(coll.insert({a: 1}));

    const fp = configureFailPoint(testDB, failpointName);
    // Pausing is only required to obtain the opId, as the target failpoint will block the build at
    // the location where we want the index build to be killed.
    IndexBuildTest.pauseIndexBuilds(primary);

    const createIdx = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});

    // When the index build starts, find its op id.
    const opId = IndexBuildTest.waitForIndexBuildToScanCollection(testDB, coll.getName(), 'a_1');

    IndexBuildTest.assertIndexBuildCurrentOpContents(testDB, opId, (op) => {
        jsTestLog('Inspecting db.currentOp() entry for index build: ' + tojson(op));
        assert.eq(
            undefined,
            op.connectionId,
            'Was expecting IndexBuildsCoordinator op; found db.currentOp() for connection thread instead: ' +
                tojson(op));
        assert.eq(
            coll.getFullName(),
            op.ns,
            'Unexpected ns field value in db.currentOp() result for index build: ' + tojson(op));
    });

    // Once we have the opId, we can resume index builds (the target failpoint will block it at the
    // desired location).
    IndexBuildTest.resumeIndexBuilds(primary);

    // Index build should be present in the config.system.indexBuilds collection.
    const indexMap =
        IndexBuildTest.assertIndexes(coll, 2, ["_id_"], ["a_1"], {includeBuildUUIDs: true});
    const indexBuildUUID = indexMap['a_1'].buildUUID;
    assert(primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}));

    // Kill the index builder thread.
    fp.wait();
    assert.commandWorked(testDB.killOp(opId));
    fp.off();

    const exitCode = createIdx({checkExitSuccess: false});
    assert.neq(
        0, exitCode, 'expected shell to exit abnormally due to index build being terminated');

    // Check that no new index has been created.  This verifies that the index build was aborted
    // rather than successfully completed.
    IndexBuildTest.assertIndexesSoon(coll, 1, ['_id_']);

    const cmdNs = testDB.getCollection('$cmd').getFullName();
    let ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.startIndexBuild': coll.getName()});
    assert.eq(1, ops.length, 'incorrect number of startIndexBuild oplog entries: ' + tojson(ops));
    ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.abortIndexBuild': coll.getName()});
    assert.eq(1, ops.length, 'incorrect number of abortIndexBuild oplog entries: ' + tojson(ops));
    ops = rst.dumpOplog(primary, {op: 'c', ns: cmdNs, 'o.commitIndexBuild': coll.getName()});
    assert.eq(0, ops.length, 'incorrect number of commitIndexBuild oplog entries: ' + tojson(ops));

    // Index build should be removed from the config.system.indexBuilds collection.
    assert.soon(() => {
        return primary.getCollection('config.system.indexBuilds').findOne({_id: indexBuildUUID}) ==
            null;
    });
}

const rst = new ReplSetTest({
    nodes: [
        {},
        {
            // Disallow elections on secondary.
            rsConfig: {
                priority: 0,
                votes: 0,
            },
        },
    ]
});
rst.startSet();
rst.initiate();

// Kill the build before it has voted for commit.
jsTestLog("killOp index build on primary before vote for commit readiness");
killopOnFailpoint(rst, 'hangAfterIndexBuildFirstDrain', 'beforeVoteCommit');

// Kill the build after it has voted for commit.
jsTestLog("killOp index build on primary after vote for commit readiness");
killopOnFailpoint(rst, 'hangIndexBuildAfterSignalPrimaryForCommitReadiness', 'afterVoteCommit');

rst.stopSet();