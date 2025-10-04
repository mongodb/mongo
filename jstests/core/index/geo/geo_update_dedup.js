// Cannot implicitly shard accessed collections because single updates are not targeted.
// @tags: [
//   assumes_unsharded_collection,
//   requires_multi_updates,
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

// Test that updates with geo queries which match
// the same document multiple times only apply
// the update once

let t = db.jstests_geo_update_dedup;

// 2d index with $near
t.drop();
t.createIndex({locs: "2d"});
t.save({
    locs: [
        [49.999, 49.999],
        [50.0, 50.0],
        [50.001, 50.001],
    ],
});

let q = {locs: {$near: [50.0, 50.0]}};
assert.eq(1, t.find(q).itcount(), "duplicates returned from query");

let res = t.update({locs: {$near: [50.0, 50.0]}}, {$inc: {touchCount: 1}}, false, true);
assert.eq(1, res.nMatched);
assert.eq(1, t.findOne().touchCount);

t.drop();
t.createIndex({locs: "2d"});
t.save({
    locs: [
        {x: 49.999, y: 49.999},
        {x: 50.0, y: 50.0},
        {x: 50.001, y: 50.001},
    ],
});
res = t.update({locs: {$near: {x: 50.0, y: 50.0}}}, {$inc: {touchCount: 1}});
assert.eq(1, res.nMatched);
assert.eq(1, t.findOne().touchCount);

// 2d index with $within
t.drop();
t.createIndex({loc: "2d"});
t.save({
    loc: [
        [0, 0],
        [1, 1],
    ],
});

res = t.update({loc: {$within: {$center: [[0, 0], 2]}}}, {$inc: {touchCount: 1}}, false, true);
assert.eq(1, res.nMatched);
assert.eq(1, t.findOne().touchCount);

// 2dsphere index with $geoNear
t.drop();
t.createIndex({geo: "2dsphere"});
let x = {
    "type": "Polygon",
    "coordinates": [
        [
            [49.999, 49.999],
            [50.0, 50.0],
            [50.001, 50.001],
            [49.999, 49.999],
        ],
    ],
};
t.save({geo: x});

res = t.update({geo: {$geoNear: {"type": "Point", "coordinates": [50.0, 50.0]}}}, {$inc: {touchCount: 1}}, false, true);
assert.eq(1, res.nMatched);
assert.eq(1, t.findOne().touchCount);

t.drop();
let locdata = [
    {geo: {type: "Point", coordinates: [49.999, 49.999]}},
    {geo: {type: "Point", coordinates: [50.0, 50.0]}},
    {geo: {type: "Point", coordinates: [50.001, 50.001]}},
];
t.save({locdata: locdata, count: 0});
t.createIndex({"locdata.geo": "2dsphere"});

res = t.update(
    {"locdata.geo": {$geoNear: {"type": "Point", "coordinates": [50.0, 50.0]}}},
    {$inc: {touchCount: 1}},
    false,
    true,
);
assert.eq(1, res.nMatched);
assert.eq(1, t.findOne().touchCount);
