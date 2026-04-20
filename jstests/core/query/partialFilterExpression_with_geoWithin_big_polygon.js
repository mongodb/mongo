// @tags: [
//  requires_fcv_90,
//  # Explain will return different plan than expected when a collection becomes a time-series
//  # collection. Also, query shape will be different.
//  exclude_from_timeseries_crud_passthrough,
// ]

// Querying with a big polygon (strict winding CRS) against a partial index with $geoWithin
// should not crash during the isSubsetOf containment check.

import {getWinningPlanFromExplain, isCollscan, isIxscan} from "jstests/libs/query/analyze_plan.js";
import {add2dsphereVersionIfNeeded} from "jstests/libs/query/geo_index_version_helpers.js";

const coll = db.partialFilterExpression_with_geoWithin_big_polygon;

const strictCRS = {type: "name", properties: {name: "urn:x-mongodb:crs:strictwinding:EPSG:4326"}};

const bigPoly20 = {
    type: "Polygon",
    coordinates: [
        [
            [10.0, 10.0],
            [-10.0, 10.0],
            [-10.0, -10.0],
            [10.0, -10.0],
            [10.0, 10.0],
        ],
    ],
    crs: strictCRS,
};

const bigPoly20Comp = {
    type: "Polygon",
    coordinates: [
        [
            [10.0, 10.0],
            [10.0, -10.0],
            [-10.0, -10.0],
            [-10.0, 10.0],
            [10.0, 10.0],
        ],
    ],
    crs: strictCRS,
};

const normalPoly = {
    type: "Polygon",
    coordinates: [
        [
            [10.0, 10.0],
            [10.0, -10.0],
            [-10.0, -10.0],
            [-10.0, 10.0],
            [10.0, 10.0],
        ],
    ],
};

// Big polygon query against a 2dsphere partial index with a normal polygon filter.
{
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, loc: {type: "Point", coordinates: [0, 0]}}));
    assert.commandWorked(coll.insert({_id: 1, loc: {type: "Point", coordinates: [50, 50]}}));

    assert.commandWorked(
        coll.createIndex(
            {loc: "2dsphere"},
            add2dsphereVersionIfNeeded({partialFilterExpression: {loc: {$geoWithin: {$geometry: normalPoly}}}}),
        ),
    );

    let results = coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}, {_id: 1}).toArray();
    assert.eq(results, [{_id: 0}]);

    // The partial index should not be selected for a big polygon query — verify collection scan.
    let explain = coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).explain("queryPlanner");
    assert(isCollscan(db, getWinningPlanFromExplain(explain)));

    // Complement big polygon (exterior of the box) should match the far point.
    results = coll.find({loc: {$geoWithin: {$geometry: bigPoly20Comp}}}, {_id: 1}).toArray();
    assert.eq(results, [{_id: 1}]);

    // Normal polygon query against the same partial index should still use the index.
    const smallPoly = {
        type: "Polygon",
        coordinates: [
            [
                [5.0, 5.0],
                [5.0, -5.0],
                [-5.0, -5.0],
                [-5.0, 5.0],
                [5.0, 5.0],
            ],
        ],
    };
    explain = coll.find({loc: {$geoWithin: {$geometry: smallPoly}}}).explain("queryPlanner");
    assert(isIxscan(db, getWinningPlanFromExplain(explain)));
}

// Big polygons on BOTH sides: partial index filter and query.
{
    coll.drop();
    assert.commandWorked(coll.insert({_id: 0, loc: {type: "Point", coordinates: [0, 0]}}));
    assert.commandWorked(coll.insert({_id: 1, loc: {type: "Point", coordinates: [50, 50]}}));

    assert.commandWorked(
        coll.createIndex({a: 1}, {partialFilterExpression: {loc: {$geoWithin: {$geometry: bigPoly20Comp}}}}),
    );

    let results = coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}, {_id: 1}).toArray();
    assert.eq(results, [{_id: 0}]);

    let explain = coll.find({loc: {$geoWithin: {$geometry: bigPoly20}}}).explain("queryPlanner");
    assert(isCollscan(db, getWinningPlanFromExplain(explain)));
}
