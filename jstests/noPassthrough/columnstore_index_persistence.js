/**
 * Tests that a columnstore index can be persisted and found in listIndexes after a server restart.
 *
 * @tags: [
 *   requires_fcv_63,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/index_catalog_helpers.js');
load("jstests/libs/columnstore_util.js");  // For setUpServerForColumnStoreIndexTest.

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

const collName = 'columnstore_index_persistence';
let db_primary = primary.getDB('test');

if (!setUpServerForColumnStoreIndexTest(db_primary)) {
    rst.stopSet();
    return;
}

let coll_primary = db_primary.getCollection(collName);

assert.commandWorked(coll_primary.createIndex({"$**": "columnstore"}));

// Restarts the primary and checks the index spec is persisted.
rst.restart(primary);
rst.waitForPrimary();
const indexList = rst.getPrimary().getDB('test').getCollection(collName).getIndexes();
assert.neq(null, IndexCatalogHelpers.findByKeyPattern(indexList, {"$**": "columnstore"}));

rst.stopSet();
})();
