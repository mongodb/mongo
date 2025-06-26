/**
 * Asserts that applying a single-phase createIndexes oplog entry for a 2d geo index with a
 * floating point bits field doesn't cause initial-sync to fail if there is an existing index
 * spec with a rounded bits value.
 * This may happen if the index was first created on the primary when the collection was empty.
 * The initial sync node would have to clone the index with the rounded value provided by
 * listIndexes and subsequently re-apply the single-phase index build entry.
 *
 * @tags: [
 *     requires_fcv_70,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

function runTest(numDocs) {
    const dbName = 'test';
    const collectionName = 'coll-' + numDocs;

    // Start one-node repl-set.
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(collectionName);

    // Add a secondary.
    const secondary = rst.add({
        rsConfig: {votes: 0, priority: 0},
        setParameter: {
            numInitialSyncAttempts: 1,
            // Increase log level to see debug messages for "Ignoring indexing error".
            logComponentVerbosity: tojsononeline({replication: 1, storage: 1}),
        },
    });

    // While the secondary is hung, we create the same index multiple times to
    // reproduce the interaction between single and two phase index builds on the
    // same index.
    const failPoint = configureFailPoint(secondary, 'initialSyncHangBeforeCopyingDatabases');
    rst.reInitiate();
    failPoint.wait();

    try {
        // Create the index using the empty collection fast path. The index build should be
        // replicated in a single createIndexes oplog entry. A floating point 'bits' option would be
        // converted to a whole number (12 in this case) when initial sync recreates the indexes on
        // the collection during the cloning phase. However, the oplog entry generated would still
        // contain the floating point value 12.345.
        assert.commandWorked(primaryColl.createIndex({a: '2d'}, {bits: 12.345}));

        for (let i = 0; i < numDocs; i++) {
            assert.commandWorked(primaryColl.insert({_id: i, a: i}));
        }
    } finally {
        // Resume initial sync. The createIndexes oplog entry will be applied.
        failPoint.off();
    }

    // Wait for initial sync to finish.
    rst.awaitSecondaryNodes();

    const attr = {
        namespace: primaryColl.getFullName(),
        spec: (spec) => {
            jsTestLog('Checking index spec in oplog application log message: ' + tojson(spec));
            return spec.name === 'a_2d';
        }
    };
    const timeoutMillis = 5 * 60 * 1000;

    // Older versions logged 7261800 for empty collections and 4718200 for non-empty. We now always
    // log 4718200 but multiversion tests can still produce the old id.
    assert.soon(
        function() {
            return checkLog.checkContainsOnceJson(secondary, 4718200, attr) ||
                checkLog.checkContainsOnceJson(secondary, 7261800, attr);
        },
        'Could not find log entries containing the id: 4718200 or 7261800, and attrs: ' +
            tojson(attr),
        timeoutMillis,
        300,
        {runHangAnalyzer: false});

    // We rely on the replica set test fixture shutdown to compare the collection contents
    // (including index specs) between the two nodes in the cluster.
    rst.stopSet();
}

// We test both empty and non-empty collections because these used to be handled slightly
// differently in the oplog application code.
runTest(0);
runTest(1);