/**
 * Tests that building partial geo indexes using the hybrid method preserves multikey information.
 *
 * @tags: [requires_document_locking, requires_replication]
 */
(function() {
    'use strict';

    load('jstests/noPassthrough/libs/index_build.js');

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
    const nodes = rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const testDB = primary.getDB('test');
    const coll = testDB.getCollection('test');

    assert.commandWorked(testDB.createCollection(coll.getName()));

    IndexBuildTest.pauseIndexBuilds(primary);

    // Create a 2dsphere partial index for documents where 'a', the field in the filter expression,
    // is greater than 0.
    const partialIndex = {b: '2dsphere'};
    const createIdx = IndexBuildTest.startIndexBuild(
        primary, coll.getFullName(), partialIndex, {partialFilterExpression: {a: {$gt: 0}}});
    IndexBuildTest.waitForIndexBuildToStart(testDB);

    // This document has an invalid geoJSON format (duplicated points), but will not be indexed.
    const unindexedDoc = {
        _id: 0,
        a: -1,
        b: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [0, 1], [0, 0]]]},
    };

    // This document has valid geoJson, and will be indexed.
    const indexedDoc = {
        _id: 1,
        a: 1,
        b: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]},
    };

    assert.commandWorked(coll.insert(unindexedDoc));
    assert.commandWorked(coll.insert(indexedDoc));

    // Removing unindexed document should succeed without error.
    assert.commandWorked(coll.remove({_id: 0}));

    IndexBuildTest.resumeIndexBuilds(primary);

    // Wait for the index build to finish.
    createIdx();
    IndexBuildTest.assertIndexes(coll, 2, ['_id_', 'b_2dsphere']);

    let res = assert.commandWorked(coll.validate({full: true}));
    assert(res.valid, 'validation failed on primary: ' + tojson(res));

    rst.stopSet();
})();
