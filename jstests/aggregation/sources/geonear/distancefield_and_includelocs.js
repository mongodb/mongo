/**
 * Tests the behavior of the $geoNear stage by varying 'distanceField' and 'includeLocs'
 * (specifically, by specifying nested fields, overriding existing fields, and so on).
 */
import {customDocumentEq} from "jstests/aggregation/extras/utils.js";

const coll = db.getCollection("geonear_distancefield_and_includelocs");
coll.drop();

/**
 * Runs an aggregation with a $geoNear stage using 'geoSpec' and an optional $project stage
 * using 'projSpec'. Returns the first result; that is, the result closest to the "near" point.
 */
function firstGeoNearResult(geoSpec, projSpec) {
    geoSpec.spherical = true;
    const pipeline = [{$geoNear: geoSpec}, {$limit: 1}];
    if (projSpec) {
        pipeline.push({$project: projSpec});
    }

    const res = coll.aggregate(pipeline).toArray();
    assert.eq(1, res.length, tojson(res));
    return res[0];
}

// Use documents with a variety of different fields: scalars, arrays, legacy points and GeoJSON
// objects.
const docWithLegacyPoint = {
    _id: "legacy",
    geo: [1, 1],
    ptForNearQuery: [1, 1],
    scalar: "foo",
    arr: [
        {a: 1, b: 1},
        {a: 2, b: 2},
    ],
};
const docWithGeoPoint = {
    _id: "point",
    geo: {type: "Point", coordinates: [1, 0]},
    ptForNearQuery: [1, 0],
    scalar: "bar",
    arr: [
        {a: 3, b: 3},
        {a: 4, b: 4},
    ],
};
const docWithGeoLine = {
    _id: "linestring",
    geo: {
        type: "LineString",
        coordinates: [
            [0, 0],
            [-1, -1],
        ],
    },
    ptForNearQuery: [-1, -1],
    scalar: "baz",
    arr: [
        {a: 5, b: 5},
        {a: 6, b: 6},
    ],
};

// We test with a 2dsphere index, since 2d indexes can't support GeoJSON objects.
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));

// Populate the collection.
assert.commandWorked(coll.insert(docWithLegacyPoint));
assert.commandWorked(coll.insert(docWithGeoPoint));
assert.commandWorked(coll.insert(docWithGeoLine));

// Define a custom way to compare documents since the results here might differ by insignificant
// amounts.
const assertCloseEnough = (left, right) =>
    assert(
        customDocumentEq({
            left: left,
            right: right,
            valueComparator: (a, b) => {
                if (typeof a !== "number") {
                    return a === b;
                }
                // Allow some minor differences in the
                // numbers.
                return Math.abs(a - b) < 1e-10;
            },
        }),
        () => `[${tojson(left)}] != [${tojson(right)}]`,
    );

[docWithLegacyPoint, docWithGeoPoint, docWithGeoLine].forEach((doc) => {
    const docPlusNewFields = (newDoc) => Object.extend(Object.extend({}, doc), newDoc);

    //
    // Tests for "distanceField".
    //
    const expectedDistance = 0.0000000000000001;

    // Test that "distanceField" can be computed in a new field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "newField"}),
        docPlusNewFields({newField: expectedDistance}),
    );

    // Test that "distanceField" can be computed in a new nested field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "nested.field"}),
        docPlusNewFields({nested: {field: expectedDistance}}),
    );

    // Test that "distanceField" can overwrite an existing scalar field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "scalar"}),
        docPlusNewFields({scalar: expectedDistance}),
    );

    // Test that "distanceField" can completely overwrite an existing array field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "arr"}),
        docPlusNewFields({arr: expectedDistance}),
    );

    // TODO (SERVER-35561): When "includeLocs" shares a path prefix with an existing field, the
    // fields are overwritten, even if they could be preserved.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "arr.b"}),
        docPlusNewFields({arr: {b: expectedDistance}}),
    );

    //
    // Tests for both "includeLocs" and "distanceField".
    //

    // Test that "distanceField" and "includeLocs" can both be specified.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "dist", includeLocs: "loc"}),
        docPlusNewFields({dist: expectedDistance, loc: doc.geo}),
    );

    // Test that "distanceField" and "includeLocs" can be the same path. The result is arbitrary
    // ("includeLocs" wins).
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "newField", includeLocs: "newField"}),
        docPlusNewFields({newField: doc.geo}),
    );

    // Test that "distanceField" and "includeLocs" are both preserved when their paths share a
    // prefix but do not conflict.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "comp.dist", includeLocs: "comp.loc"}),
        docPlusNewFields({comp: {dist: expectedDistance, loc: doc.geo}}),
    );

    //
    // Tests for "includeLocs" only. Project out the distance field.
    //
    const removeDistFieldProj = {d: 0};

    // Test that "includeLocs" can be computed in a new field.
    assertCloseEnough(
        firstGeoNearResult(
            {near: doc.ptForNearQuery, distanceField: "d", includeLocs: "newField"},
            removeDistFieldProj,
        ),
        docPlusNewFields({newField: doc.geo}),
    );

    // Test that "includeLocs" can be computed in a new nested field.
    assertCloseEnough(
        firstGeoNearResult(
            {near: doc.ptForNearQuery, distanceField: "d", includeLocs: "nested.field"},
            removeDistFieldProj,
        ),
        docPlusNewFields({nested: {field: doc.geo}}),
    );

    // Test that "includeLocs" can overwrite an existing scalar field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "d", includeLocs: "scalar"}, removeDistFieldProj),
        docPlusNewFields({scalar: doc.geo}),
    );

    // Test that "includeLocs" can completely overwrite an existing array field.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "d", includeLocs: "arr"}, removeDistFieldProj),
        docPlusNewFields({arr: doc.geo}),
    );

    // TODO (SERVER-35561): When "includeLocs" shares a path prefix with an existing field, the
    // fields are overwritten, even if they could be preserved.
    assertCloseEnough(
        firstGeoNearResult({near: doc.ptForNearQuery, distanceField: "d", includeLocs: "arr.a"}, removeDistFieldProj),
        docPlusNewFields({arr: {a: doc.geo}}),
    );
});
