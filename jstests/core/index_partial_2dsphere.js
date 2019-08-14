// A document with invalid GeoJSON should be able to be removed or updated when it is not part of
// a partial index filter.
// @tags: [requires_non_retryable_writes]

(function() {

"use strict";

let coll = db.index_partial_2dsphere;
coll.drop();

// Create a 2dsphere partial index for documents where isIndexed is greater than 0.
let partialIndex = {geoJson: '2dsphere'};
assert.commandWorked(
    coll.createIndex(partialIndex, {partialFilterExpression: {isIndexed: {$gt: 0}}}));

// This document has an invalid geoJSON format (duplicated points), but will not be indexed.
let unindexedDoc = {
    "_id": 0,
    "isIndexed": -1,
    "geoJson": {"type": "Polygon", "coordinates": [[[0, 0], [0, 1], [1, 1], [0, 1], [0, 0]]]}
};

// This document has valid geoJson, and will be indexed.
let indexedDoc = {
    "_id": 1,
    "isIndexed": 1,
    "geoJson": {"type": "Polygon", "coordinates": [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]}
};

assert.commandWorked(coll.insert(unindexedDoc));
assert.commandWorked(coll.insert(indexedDoc));

// Return the one indexed document.
assert.eq(
    1,
    coll.find(
            {isIndexed: 1, geoJson: {$geoNear: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .itcount());

// Don't let an update to a document with an invalid geoJson succeed.
assert.writeError(coll.update({_id: 0}, {$set: {isIndexed: 1}}));

// Update the indexed document to remove it from the index.
assert.commandWorked(coll.update({_id: 1}, {$set: {isIndexed: -1}}));

// This query should now return zero documents.
assert.eq(
    0,
    coll.find(
            {isIndexed: 1, geoJson: {$geoNear: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .itcount());

// Re-index the document.
assert.commandWorked(coll.update({_id: 1}, {$set: {isIndexed: 1}}));

// Remove both should succeed without error.
assert.commandWorked(coll.remove({_id: 0}));
assert.commandWorked(coll.remove({_id: 1}));

assert.eq(
    0,
    coll.find(
            {isIndexed: 1, geoJson: {$geoNear: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .itcount());
assert.commandWorked(coll.dropIndex(partialIndex));
})();
