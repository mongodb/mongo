// @tags: [
//   requires_getmore,
// ]

let t = db.geo_box2;

t.drop();

for (let i = 1; i < 10; i++) {
    for (let j = 1; j < 10; j++) {
        t.insert({loc: [i, j]});
    }
}

t.createIndex({"loc": "2d"});
assert.eq(
    9,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [4, 4],
                        [6, 6],
                    ],
                },
            },
        })
        .itcount(),
    "A1",
);

t.dropIndex({"loc": "2d"});

t.createIndex({"loc": "2d"}, {"min": 0, "max": 10});
assert.eq(
    9,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [4, 4],
                        [6, 6],
                    ],
                },
            },
        })
        .itcount(),
    "B1",
);
