/**
 * Tests listCatalog aggregation stage with local and majority read concerns.
 * @tags: [
 *     requires_majority_read_concern,
 *     requires_replication,
 * ]
 */
(function() {
'use strict';

load("jstests/libs/fail_point_util.js");  // For configureFailPoint

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const documentSourceListCatalogEnabled =
    assert
        .commandWorked(
            primary.adminCommand({getParameter: 1, featureFlagDocumentSourceListCatalog: 1}))
        .featureFlagDocumentSourceListCatalog.value;

if (!documentSourceListCatalogEnabled) {
    jsTestLog('Skipping test because the $listCatalog aggregation stage feature flag is disabled.');
    rst.stopSet();
    return;
}

const testDB = primary.getDB('test');
const coll = testDB.getCollection('t');
assert.commandWorked(coll.insert({_id: 0}));
const view = testDB.getCollection('view1');
assert.commandWorked(testDB.createView(view.getName(), coll.getName(), []));
rst.awaitReplication();

const secondaries = rst.getSecondaries();
assert.eq(2, secondaries.length);

let failpoints = [];
try {
    failpoints.push(configureFailPoint(secondaries[0], 'rsSyncApplyStop'));
    failpoints.push(configureFailPoint(secondaries[1], 'rsSyncApplyStop'));

    const collOnPrimaryOnly = testDB.getCollection('w');
    assert.commandWorked(collOnPrimaryOnly.insert({_id: 1}, {writeConcern: {w: 1}}));

    const viewOnPrimaryOnly = testDB.getCollection('view2');
    assert.commandWorked(
        testDB.createView(viewOnPrimaryOnly.getName(), coll.getName(), [], {writeConcern: {w: 1}}));

    const adminDB = testDB.getSiblingDB('admin');
    const resultLocal = adminDB
                            .aggregate([{$listCatalog: {}}, {$match: {db: testDB.getName()}}],
                                       {readConcern: {level: 'local'}})
                            .toArray();
    const resultMajority = adminDB
                               .aggregate([{$listCatalog: {}}, {$match: {db: testDB.getName()}}],
                                          {readConcern: {level: 'majority'}})
                               .toArray();

    jsTestLog('$listCatalog result (local read concern): ' + tojson(resultLocal));
    jsTestLog('$listCatalog result (majority read concern): ' + tojson(resultMajority));

    const catalogEntriesLocal = Object.assign({}, ...resultLocal.map(doc => ({[doc.ns]: doc})));
    const catalogEntriesMajority =
        Object.assign({}, ...resultMajority.map(doc => ({[doc.ns]: doc})));
    jsTestLog('Catalog entries keyed by namespace (local read concern): ' +
              tojson(catalogEntriesLocal));
    jsTestLog('Catalog entries keyed by namespace (majority read concern): ' +
              tojson(catalogEntriesMajority));

    // $listCatalog result should have all the collections and views we have created.
    assert.hasFields(catalogEntriesLocal, [
        coll.getFullName(),
        view.getFullName(),
        collOnPrimaryOnly.getFullName(),
        viewOnPrimaryOnly.getFullName()
    ]);

    // $listCatalog result should not contain the namespaces not replicated to the secondaries.
    assert.hasFields(catalogEntriesMajority, [coll.getFullName(), view.getFullName()]);
    assert(!catalogEntriesMajority.hasOwnProperty(collOnPrimaryOnly.getFullName()),
           tojson(catalogEntriesMajority));
    assert(!catalogEntriesMajority.hasOwnProperty(viewOnPrimaryOnly.getFullName()),
           tojson(catalogEntriesMajority));
} finally {
    for (const fp of failpoints) {
        fp.off();
    }
}

rst.stopSet();
})();
