/**
 * Polygons with strict-winding CRS are not allowed to be indexed in a geo index (to avoid index
 * key explosion for very large geometries). This also applies to a GeometryCollection that
 * contains a strict-winding polygon as part of its collection.
 *
 * This file tests that a GeometryCollection with a strict-winding polygon is rejected and accepted
 * appropriately in different contexts:
 *   1. Insert into unindexed collection is accepted
 *   2. Insert into 2dsphere-indexed collection is rejected
 *   3. createIndex on collection that already has the document is rejected
 *   4. Querying a collection that stores strict-winding geometry: the strict-winding document is
 *      not matched, while a normal document in the same collection is returned correctly
 *
 * @tags: [
 *      requires_non_retryable_writes,
 *      does_not_support_causal_consistency,
 *      multiversion_incompatible,
 * ]
 */

import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

const strictCRS = {
    type: "name",
    properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"},
};

// Note that since the crs is strict and the coordinates traverse counter-clockwise, the polygon's
// interior here is the space outside of the defined 5x5 area.
const strictGeoCollection = {
    type: "GeometryCollection",
    geometries: [
        {
            type: "Polygon",
            coordinates: [
                [
                    [0, 0],
                    [5, 0],
                    [5, 5],
                    [0, 5],
                    [0, 0],
                ],
            ],
            crs: strictCRS,
        },
    ],
};

// A normal polygon (default CRS) that overlaps with the strict-winding polygon coordinates.
const normalPolygon = {
    type: "Polygon",
    coordinates: [
        [
            [1, 1],
            [3, 1],
            [3, 3],
            [1, 3],
            [1, 1],
        ],
    ],
};

const queryPolygon = {
    type: "Polygon",
    coordinates: [
        [
            [0, 0],
            [10, 0],
            [10, 10],
            [0, 10],
            [0, 0],
        ],
    ],
};

const withinPolygon = {
    type: "Polygon",
    coordinates: [
        [
            [-10, -10],
            [10, -10],
            [10, 10],
            [-10, 10],
            [-10, -10],
        ],
    ],
};

// ── insert + query on unindexed collection ──────────────────────────────────────────────────────
// Without a 2dsphere index, no index keys are generated at insert time, so the document is
// accepted regardless of its CRS. When queried via a collection scan, the matcher recognises the
// strict-winding CRS and produces no matches for that document. A normal document in the same
// collection is matched correctly, confirming the query works and not just silently skips all docs.
{
    const coll = db.geo_strict_polygon_gc_no_index;
    coll.drop();

    // Insert both a strict-winding and a normal document.
    assert.commandWorked(coll.insert({_id: 1, geo: strictGeoCollection}));
    assert.commandWorked(coll.insert({_id: 2, geo: normalPolygon}));

    const indexes = coll.getIndexes();
    assert(!indexes.some((idx) => idx.key && idx.key.geo === "2dsphere"), "Expected no 2dsphere index");

    // $geoIntersects returns only the normal document — the strict-winding doc is skipped, but
    // the query is functional and returns correct results.
    const intersectResults = coll.find({geo: {$geoIntersects: {$geometry: queryPolygon}}}).toArray();
    assert.eq(intersectResults.length, 1, "$geoIntersects should match only the normal document");
    assert.eq(intersectResults[0]._id, 2, "matched document should be the normal polygon");

    // $geoWithin likewise skips the strict-winding doc and matches the normal one.
    const withinResults = coll.find({geo: {$geoWithin: {$geometry: withinPolygon}}}).toArray();
    assert.eq(withinResults.length, 1, "$geoWithin should match only the normal document");
    assert.eq(withinResults[0]._id, 2, "matched document should be the normal polygon");

    coll.drop();
}

// ── insert into 2dsphere-indexed collection ─────────────────────────────────────────────────────
// A 2dsphere index generates S2 covering keys for each indexed geometry. Strict-winding polygons
// are excluded from indexing because they can produce an unbounded number of covering cells.
// Inserting such a document is therefore rejected gracefully rather than crashing.
{
    const coll = db.geo_strict_polygon_gc_with_index;
    coll.drop();
    assert.commandWorked(coll.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded()));

    // Insert into an indexed collection must be rejected.
    assert.commandFailedWithCode(
        db.runCommand({insert: coll.getName(), documents: [{_id: 1, geo: strictGeoCollection}]}),
        16755,
        "insert of strict-winding GeometryCollection into 2dsphere-indexed collection should fail",
    );

    assert.commandWorked(coll.dropIndexes(), "drop indexes");
    coll.drop();
}

// ── createIndex on collection that already has the document ─────────────────────────────────────
// The same key-generation restriction applies during an index build. If a collection already
// contains a strict-winding polygon (directly or nested in a GeometryCollection), the index build
// fails and no partial index is left behind.
{
    const coll = db.geo_strict_polygon_gc_index_build;
    coll.drop();

    // Pre-populate the collection without an index (insert is accepted).
    assert.commandWorked(coll.insert({_id: 1, geo: strictGeoCollection}));

    // createIndex must be rejected because the existing document cannot be indexed.
    assert.commandFailedWithCode(
        coll.createIndex({geo: "2dsphere"}, add2dsphereVersionIfNeeded()),
        16755,
        "createIndex on collection containing strict-winding GeometryCollection should fail",
    );

    // The index must not have been created.
    assert.soon(
        () => !coll.getIndexes().some((idx) => idx.key && idx.key.geo === "2dsphere"),
        "2dsphere index must not exist after failed createIndex",
    );

    coll.drop();
}
