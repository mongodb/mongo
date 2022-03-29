/**
 * Tests the $listCatalog aggregation stage with local and majority read concerns.
 *
 * @tags: [
 *     requires_majority_read_concern,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/libs/write_concern_util.js');

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB(jsTestName());
const coll1 = db.coll_1;
const coll2 = db.coll_2;
const view = db.view;

assert.commandWorked(
    db.runCommand({createIndexes: coll1.getName(), indexes: [{key: {a: 1}, name: 'a_1'}]}));
assert.commandWorked(
    db.runCommand({create: view.getName(), viewOn: coll1.getName(), pipeline: []}));

stopReplicationOnSecondaries(rst);

assert.commandWorked(db.runCommand({
    createIndexes: coll1.getName(),
    indexes: [{key: {b: 1}, name: 'b_1'}],
    writeConcern: {w: 1},
    commitQuorum: 0,
}));
assert.commandWorked(db.runCommand({create: coll2.getName(), writeConcern: {w: 1}}));
assert.commandWorked(db.runCommand({collMod: view.getName(), viewOn: coll2.getName()}));

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

rst.stopSet();
})();
