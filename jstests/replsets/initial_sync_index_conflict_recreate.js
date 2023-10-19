/**
 * Test that a collection cloned during an index build doesn't cause issues with concurrent index
 * operations referring to the same index.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

const dbName = 'test';
const collectionName = 'coll';

const times = [
    ISODate('1970-01-01T00:00:00'),
    ISODate('1970-01-01T00:00:07'),
];
let docs = [];
for (const m of ['a', 'A', 'b', 'B'])
    for (const t of times)
        docs.push({t, m});

// Start one-node repl-set.
const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);

// Add a secondary.
const secondary = rst.add({
    rsConfig: {votes: 0, priority: 0},
    setParameter: {numInitialSyncAttempts: 1, logComponentVerbosity: '{verbosity: 1}'},
});

// Make the secondary pause once it has scanned the source's oplog. This will cause the secondary to
// apply all oplog entries from now on.
const failPoint = configureFailPoint(secondary, 'initialSyncHangBeforeCopyingDatabases');
rst.reInitiate();
failPoint.wait();

// The secondary has established the replay oplog begin timestamp. Proceed to create a collection.
primaryDB.createCollection(collectionName, {
    collation: {locale: 'en_US', strength: 2},
});

const primaryColl = primaryDB.getCollection(collectionName);
// This will create a createIndexes oplog entry.
primaryColl.createIndex({m: 1, t: 1}, {
    collation: {locale: 'simple'},
});
// Insert some documents so that another createIndexes becomes a two-phase index build.
assert.commandWorked(primaryColl.insert(docs));

// We will now drop all indexes and recreate them. We will also prevent the primary from completing
// the index build. This will cause the secondary to detect an unfinished index build during the
// cloning phase.
primaryColl.dropIndexes();
IndexBuildTest.pauseIndexBuilds(primary);
const createIdx = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {m: 1, t: 1}, {
    collation: {locale: 'simple'},
});
IndexBuildTest.waitForIndexBuildToStart(primaryDB, primaryColl.getName(), "m_1_t_1");

// Continue cloning, this will make the unfinished index build be processed by the CollectionCloner
// on the secondary. Pause the secondary before performing oplog replay.
const oplogApplierFp = configureFailPoint(secondary, 'initialSyncHangAfterDataCloning');
failPoint.off();
oplogApplierFp.wait();
// Complete the index build.
IndexBuildTest.resumeIndexBuilds(primary);
oplogApplierFp.off();
// There are now an unfinished index build + createIndex oplog entry on the secondary. Both entries
// refer to the same index, wait until it's caught up.
rst.awaitReplication();
createIdx();
rst.stopSet();
