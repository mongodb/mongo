// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
//   requires_getmore,
// ]

// Make sure nesting of location arrays also works.

let t = db.geonest;
t.drop();

t.insert({
    zip: "10001",
    data: [
        {loc: [10, 10], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});
t.insert({
    zip: "10002",
    data: [
        {loc: [20, 20], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});
let res = t.insert({
    zip: "10003",
    data: [
        {loc: [30, 30], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});
assert.commandWorked(res);

assert.commandWorked(t.createIndex({"data.loc": "2d", zip: 1}));
assert.eq(2, t.getIndexKeys().length);

res = t.insert({
    zip: "10004",
    data: [
        {loc: [40, 40], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});
assert.commandWorked(res);

// test normal access

printjson(
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [0, 0],
                        [45, 45],
                    ],
                },
            },
        })
        .toArray(),
);

assert.eq(
    4,
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [0, 0],
                        [45, 45],
                    ],
                },
            },
        })
        .count(),
);

assert.eq(
    4,
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [45, 45],
                        [50, 50],
                    ],
                },
            },
        })
        .count(),
);

// Try a complex nesting

t = db.geonest;
t.drop();

t.insert({zip: "10001", data: [{loc: [[10, 10], {lat: 50, long: 50}], type: "home"}]});
t.insert({
    zip: "10002",
    data: [
        {loc: [20, 20], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});
res = t.insert({zip: "10003", data: [{loc: [{x: 30, y: 30}, [50, 50]], type: "home"}]});
assert(!res.hasWriteError());

assert.commandWorked(t.createIndex({"data.loc": "2d", zip: 1}));
assert.eq(2, t.getIndexKeys().length);

res = t.insert({
    zip: "10004",
    data: [
        {loc: [40, 40], type: "home"},
        {loc: [50, 50], type: "work"},
    ],
});

assert.commandWorked(res);

// test normal access
printjson(
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [0, 0],
                        [45, 45],
                    ],
                },
            },
        })
        .toArray(),
);

assert.eq(
    4,
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [0, 0],
                        [45, 45],
                    ],
                },
            },
        })
        .count(),
);

assert.eq(
    4,
    t
        .find({
            "data.loc": {
                $within: {
                    $box: [
                        [45, 45],
                        [50, 50],
                    ],
                },
            },
        })
        .count(),
);
