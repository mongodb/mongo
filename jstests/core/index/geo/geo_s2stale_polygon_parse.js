/**
 * Tests for 2dsphere index version 4 behavior, specifically the v4 GeoJSON-first parse path
 * with legacy point fallback.
 *
 * @tags: [
 *   backport_required_multiversion,
 *   does_not_support_stepdowns,
 *   featureFlag2dsphereIndexVersion4,
 *   cannot_run_during_upgrade_downgrade,
 *   multiversion_incompatible
 * ]
 */

import {describe, it} from "jstests/libs/mochalite.js";

const collName = jsTestName();

describe("2dsphere index version 4", function () {
    const coll = db[collName];

    // Regression test: inserting a document with a strict-winding Polygon whose "coordinates"
    // field is a scalar (not an array) must not crash the server. In a v4 index the parse path
    // tries GeoJSON first; when that fails it falls back to legacy point parsing. A bug left
    // _polygon partially initialized (bigPolygon allocated but _loop == nullptr), which caused
    // a null dereference in BigSimplePolygon::GetCapBound() during index key generation.
    it("indexes doc with strict-winding Polygon and scalar coordinates as legacy point", function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({loc: "2dsphere"}, {"2dsphereIndexVersion": 4}));

        // The document has numeric first-two fields (triggers legacy point fallback in v4),
        // type Polygon with strict-winding CRS, and a non-array "coordinates" field.
        const doc = {
            loc: {
                a: 1,
                b: 2,
                type: "Polygon",
                crs: {
                    type: "name",
                    properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"},
                },
                coordinates: 0,
            },
        };

        // GeoJSON parsing fails (coordinates is not an array); _polygon is reset cleanly;
        // legacy point fallback succeeds on {a:1, b:2}. Insert must succeed.
        assert.commandWorked(coll.insert(doc));
        assert.eq(1, coll.find({}).itcount(), "expected inserted doc to be found", {doc});
    });
});
