/**
 * Tests the $listCatalog aggregation stage with local and majority read concerns.
 *
 * @tags: [
 *     requires_majority_read_concern,
 *     requires_replication,
 *     requires_snapshot_read,
 * ]
 */
import {
    restartReplicationOnSecondaries,
    stopReplicationOnSecondaries
} from "jstests/libs/write_concern_util.js";

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        setParameter: {
            // Set the history window to 1 hour to prevent the oldest timestamp from advancing. This
            // is necessary to avoid removing data files across restarts for this test.
            minSnapshotHistoryWindowInSeconds: 60 * 60,
        }
    },
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());
const coll1 = db.coll_1;
const coll2 = db.coll_2;
const view = db.view;

assert.commandWorked(
    db.runCommand({createIndexes: coll1.getName(), indexes: [{key: {a: 1}, name: 'a_1'}]}));
const viewTS = assert
                   .commandWorked(db.runCommand(
                       {create: view.getName(), viewOn: coll1.getName(), pipeline: []}))
                   .operationTime;

jsTestLog(view.getFullName() + ' view on ' + coll1.getFullName() +
          ' created at: ' + tojson(viewTS) + ' (viewTS)');

stopReplicationOnSecondaries(rst);

assert.commandWorked(db.runCommand({
    createIndexes: coll1.getName(),
    indexes: [{key: {b: 1}, name: 'b_1'}],
    writeConcern: {w: 1},
    commitQuorum: 0,
}));

assert.commandWorked(db.runCommand({create: coll2.getName(), writeConcern: {w: 1}}));
const collModTS =
    assert.commandWorked(db.runCommand({collMod: view.getName(), viewOn: coll2.getName()}))
        .operationTime;

jsTestLog(view.getFullName() + ' view redirected from ' + coll1.getFullName() + ' to ' +
          coll2.getFullName() + ' at: ' + tojson(collModTS) + ' (collModTS)');

let entries = coll1.aggregate([{$listCatalog: {}}], {readConcern: {level: 'local'}}).toArray();
jsTestLog(coll1.getFullName() + ' local $listCatalog: ' + tojson(entries));
assert.eq(entries.length, 1);
assert.eq(entries[0].ns, coll1.getFullName());
assert.eq(entries[0].md.indexes.length, 3);

entries = coll1.aggregate([{$listCatalog: {}}], {readConcern: {level: 'majority'}}).toArray();
jsTestLog(coll1.getFullName() + ' majority $listCatalog: ' + tojson(entries));
assert.eq(entries.length, 1);
assert.eq(entries[0].ns, coll1.getFullName());
assert.eq(entries[0].md.indexes.length, 2);

const adminDB = primary.getDB('admin');

entries = adminDB
              .aggregate([{$listCatalog: {}}, {$match: {db: db.getName()}}],
                         {readConcern: {level: 'local'}})
              .toArray();
jsTestLog('Collectionless local $listCatalog: ' + tojson(entries));
assert.eq(entries.length, 4);
assert.eq(entries.find((entry) => entry.name === view.getName()).viewOn, coll2.getName());

entries = adminDB
              .aggregate([{$listCatalog: {}}, {$match: {db: db.getName()}}],
                         {readConcern: {level: 'majority'}})
              .toArray();
jsTestLog('Collectionless majority $listCatalog: ' + tojson(entries));
assert.eq(entries.length, 3);
assert.eq(entries.find((entry) => entry.name === view.getName()).viewOn, coll1.getName());

restartReplicationOnSecondaries(rst);

// Check $listCatalog behavior using point-in-time lookups.
// Since 'viewTS' corresponds to the last operation before we initially suspended replication,
// PIT reads at 'collModTS' should match 'local' results; while PIT reads at 'viewBTS' should
// be consistent with 'majority' results.

rst.awaitReplication();

// At 'collModTS' cluster time, {b: 1} has been created, hence the additional index returned in
// 'md.indexes'.
entries = coll1
              .aggregate([{$listCatalog: {}}],
                         {readConcern: {level: 'snapshot', atClusterTime: collModTS}})
              .toArray();
jsTestLog(coll1.getFullName() + ' snapshot $listCatalog (at collModTS: ' + tojson(collModTS) +
          '):  ' + tojson(entries));
assert.eq(entries.length, 1);
assert.eq(entries[0].ns, coll1.getFullName());
assert.eq(entries[0].md.indexes.length, 3);

// At 'viewTS' cluster time, we have not created the {b: 1} index on 'coll1', so the only
// indexes returned by $listCatalog will be {_id: 1} and {a: 1}.
entries =
    coll1.aggregate([{$listCatalog: {}}], {readConcern: {level: 'snapshot', atClusterTime: viewTS}})
        .toArray();
jsTestLog(coll1.getFullName() + ' snapshot $listCatalog (at viewTS: ' + tojson(viewTS) +
          '): ' + tojson(entries));
assert.eq(entries.length, 1);
assert.eq(entries[0].ns, coll1.getFullName());
assert.eq(entries[0].md.indexes.length, 2);

// At 'collModTS' cluster time, we modified the view and it should point to 'coll2'.
entries = adminDB
              .aggregate([{$listCatalog: {}}, {$match: {db: db.getName()}}],
                         {readConcern: {level: 'snapshot', atClusterTime: collModTS}})
              .toArray();
jsTestLog('Collectionless snapshot $listCatalog (at collModTS: ' + tojson(collModTS) +
          '): ' + tojson(entries));
assert.eq(entries.length, 4);
assert.eq(entries.find((entry) => entry.name === view.getName()).viewOn, coll2.getName());

// At 'viewTS' cluster time, we had just created the view and it should point to 'coll1'.
// Collection 'coll2' has not been created at this cluster time, hence the lower entry count.
entries = adminDB
              .aggregate([{$listCatalog: {}}, {$match: {db: db.getName()}}],
                         {readConcern: {level: 'snapshot', atClusterTime: viewTS}})
              .toArray();
jsTestLog('Collectionless snapshot $listCatalog (at viewTS: ' + tojson(viewTS) +
          '): ' + tojson(entries));
assert.eq(entries.length, 3);
assert.eq(entries.find((entry) => entry.name === view.getName()).viewOn, coll1.getName());

rst.stopSet();
