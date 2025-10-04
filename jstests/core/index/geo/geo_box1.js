// @tags: [
//   requires_getmore,
// ]

let t = db.geo_box1;
t.drop();

let num = 0;
for (let x = 0; x <= 20; x++) {
    for (let y = 0; y <= 20; y++) {
        let o = {_id: num++, loc: [x, y]};
        t.save(o);
    }
}

t.createIndex({loc: "2d"});

let searches = [
    [
        [1, 2],
        [4, 5],
    ],
    [
        [1, 1],
        [2, 2],
    ],
    [
        [0, 2],
        [4, 5],
    ],
    [
        [1, 1],
        [2, 8],
    ],
];

for (let i = 0; i < searches.length; i++) {
    let b = searches[i];
    // printjson( b );

    let q = {loc: {$within: {$box: b}}};
    let numWanetd = (1 + b[1][0] - b[0][0]) * (1 + b[1][1] - b[0][1]);
    assert.eq(numWanetd, t.find(q).itcount(), "itcount: " + tojson(q));
    printjson(t.find(q).explain());
}

assert.eq(
    0,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [100, 100],
                        [110, 110],
                    ],
                },
            },
        })
        .itcount(),
    "E1",
);
assert.eq(
    0,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [100, 100],
                        [110, 110],
                    ],
                },
            },
        })
        .count(),
    "E2",
);

assert.eq(
    num,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [0, 0],
                        [110, 110],
                    ],
                },
            },
        })
        .count(),
    "E3",
);
assert.eq(
    num,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [0, 0],
                        [110, 110],
                    ],
                },
            },
        })
        .itcount(),
    "E4",
);

assert.eq(
    57,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [0, 0],
                        [110, 110],
                    ],
                },
            },
        })
        .limit(57)
        .itcount(),
    "E5",
);

// SERVER-13621
// Eetect and invert the $box coordinates when they're specified incorrectly.
assert.eq(
    num,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [110, 110],
                        [0, 0],
                    ],
                },
            },
        })
        .count(),
    "E5",
);
assert.eq(
    num,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [110, 0],
                        [0, 110],
                    ],
                },
            },
        })
        .count(),
    "E6",
);
assert.eq(
    num,
    t
        .find({
            loc: {
                $within: {
                    $box: [
                        [0, 110],
                        [110, 0],
                    ],
                },
            },
        })
        .count(),
    "E7",
);
