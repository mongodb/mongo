/**
 * Tests that when directoryperdb is enabled, newly empty database directories are removed for
 * replicated collection with two-phase drops.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbToDropName = jsTestName() + "_drop";
const dbToKeepName = jsTestName() + "_keep";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {directoryperdb: "", setParameter: {minSnapshotHistoryWindowInSeconds: 0}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbToDrop = primary.getDB(dbToDropName);
const dbToKeep = primary.getDB(dbToKeepName);
const collToKeep = dbToKeep.getCollection(jsTestName());

const runTest = function (dropDatabase) {
    assert.commandWorked(primary.adminCommand({clearLog: "global"}));

    const collToDrop = dbToDrop.getCollection(jsTestName() + "_1");
    assert.commandWorked(collToDrop.insert({a: 1}));

    if (dropDatabase) {
        assert.commandWorked(dbToDrop.dropDatabase());
    } else {
        assert(collToDrop.drop());
    }

    // Move the oldest_timestamp forward past the drop timestamp and take a checkpoint so that the
    // second phase of the collection drop can occur.
    assert.commandWorked(collToKeep.insert({}));
    rst.awaitLastOpCommitted();
    assert.commandWorked(primary.adminCommand({fsync: 1}));

    // Ensure that the empty database directory was removed.
    checkLog.containsJson(primary, 4888200);
    const files = listFiles(rst.getDbPath(primary));
    assert(
        !files.some((file) => file.baseName === dbToDropName),
        "Database directory " + dbToDropName + " found even though it should have been removed: " + tojson(files),
    );
};

runTest(false);
runTest(true);

rst.stopSet();
