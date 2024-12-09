/**
 * With a normal ascending index on a geospatial field, {$not: {$geoWithin: <>}} and
 * {$not: {$geoIntersects: <>}} queries should return proper results. Previously, those queries
 * failed due to attempting to build geospatial index bounds on the non-geo index. See SERVER-92193
 * for more details.
 */
(function() {
"use strict";

const coll = db.negated_geo_queries;
coll.drop();

assert.commandWorked(coll.insert(
    {loc: {type: "Polygon", coordinates: [[[0, 0], [0, 1], [1, 1], [1, 0], [0, 0]]]}, a: 0}));
assert.commandWorked(coll.insert(
    {loc: {type: "Polygon", coordinates: [[[30, 0], [30, 1], [32, 1], [32, 0], [30, 0]]]}, a: 0}));
assert.commandWorked(coll.insert(
    {loc: {type: "Polygon", coordinates: [[[-55, 10], [-35, 15], [-45, 10], [-55, 10]]]}, a: 1}));

const targetPolygonWithin = {
    type: "Polygon",
    coordinates: [[[-5, -5], [5, -5], [5, 5], [-5, 5], [-5, -5]]]
};

const targetLineIntersect = {
    type: "LineString",
    coordinates: [[29, -1], [31, 1]]
};

function runTest() {
    // Tests $geoWithin.
    let geoQuery = {loc: {$geoWithin: {$geometry: targetPolygonWithin}}};
    let res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);

    // Tests $geoWithin under a $not.
    geoQuery = {loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}};
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 2);

    // Tests $geoIntersects.
    geoQuery = {loc: {$geoIntersects: {$geometry: targetLineIntersect}}};
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);

    // Tests $geoIntersects under a $not.
    geoQuery = {loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}};
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 2);

    // Tests an $and of the negated $geoWithin and negated $geoIntersects.
    geoQuery = {
        $and: [
            {loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}},
            {loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}}
        ]
    };
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);

    // Tests the same logic as above written with a $nor.
    geoQuery = {
        $nor: [
            {loc: {$geoWithin: {$geometry: targetPolygonWithin}}},
            {loc: {$geoIntersects: {$geometry: targetLineIntersect}}}
        ]
    };
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);

    // Tests an $and of the negated $geoWithin and an expression on a non-geo field.
    geoQuery = {$and: [{loc: {$not: {$geoWithin: {$geometry: targetPolygonWithin}}}}, {a: 0}]};
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);

    // Tests an $and of the negated $geoIntersects and an expression on a non-geo field.
    geoQuery = {$and: [{loc: {$not: {$geoIntersects: {$geometry: targetLineIntersect}}}}, {a: 0}]};
    res = coll.find(geoQuery);
    assert.eq(res.itcount(), 1);
}

// Run test with just a simple btree index.
assert.commandWorked(coll.createIndex({loc: 1}));
runTest();

// Run test with a simple btree index and a geo index.
assert.commandWorked(coll.createIndex({loc: "2dsphere"}));
runTest();

// Run test with just a geo index.
assert.commandWorked(coll.dropIndex({loc: 1}));
runTest();
})();
