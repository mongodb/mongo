/**
 * Tests dropping the bucket collection still results in a collection existing and being droppable
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_getmore,
 *   ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    const coll = testDB.timeseries_bucket_drop;
    const buckets = testDB.getCollection('system.buckets.' + coll.getName());
    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));
    // Drop the buckets first
    assert.commandWorked(testDB.runCommand({drop: buckets.getName()}));
    let collections =
        assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
    // Check that we delete bucket but not collection
    assert.isnull(collections.find(entry => entry.name == buckets.getName()));
    assert(collections.find(entry => entry.name == coll.getName()));
    // Still should be able to drop the collection
    assert.commandWorked(testDB.runCommand({drop: coll.getName()}));
    collections = assert.commandWorked(testDB.runCommand({listCollections: 1})).cursor.firstBatch;
    assert.isnull(collections.find(entry => entry.name == buckets.getName()));
    assert.isnull(collections.find(entry => entry.name == coll.getName()));
});
})();
