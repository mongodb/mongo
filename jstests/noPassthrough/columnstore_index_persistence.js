/**
 * Tests that a columnstore index can be persisted and found in listIndexes after a server restart.
 *
 * @tags: [
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/index_catalog_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
const columnstoreIndexesEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagColumnstoreIndexes: 1}))
        .featureFlagColumnstoreIndexes.value;

if (!columnstoreIndexesEnabled) {
    jsTestLog('Skipping test because the columnstore index feature flag is disabled');
    rst.stopSet();
    return;
}

const collName = 'columnstore_index_persistence';
let db_primary = primary.getDB('test');
let coll_primary = db_primary.getCollection(collName);

assert.commandWorked(coll_primary.createIndex({"$**": "columnstore"}));

// Restarts the primary and checks the index spec is persisted.
rst.restart(primary);
rst.waitForPrimary();
const indexList = rst.getPrimary().getDB('test').getCollection(collName).getIndexes();
assert.neq(null, IndexCatalogHelpers.findByKeyPattern(indexList, {"$**": "columnstore"}));

rst.stopSet();
})();
