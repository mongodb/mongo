//
// @tags: [SERVER-40561]
//

let t = db.geo_polygon4;
t.drop();

let num = 0;
let bulk = t.initializeUnorderedBulkOp();
for (let x = -180; x < 180; x += 0.5) {
    for (let y = -180; y < 180; y += 0.5) {
        let o = {_id: num++, loc: [x, y]};
        bulk.insert(o);
    }
}
assert.commandWorked(bulk.execute());

let numTests = 31;
for (let n = 0; n < numTests; n++) {
    t.dropIndexes();
    t.createIndex({loc: "2d"}, {bits: 2 + n});

    assert.between(
        9 - 2,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [0, 0],
                            [1, 1],
                            [0, 2],
                        ],
                    },
                },
            })
            .count(),
        9,
        "Triangle Test",
        true,
    );
    assert.eq(
        num,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [-180, -180],
                            [-180, 180],
                            [180, 180],
                            [180, -180],
                        ],
                    },
                },
            })
            .count(),
        "Bounding Box Test",
    );

    assert.eq(
        441,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [0, 0],
                            [0, 10],
                            [10, 10],
                            [10, 0],
                        ],
                    },
                },
            })
            .count(),
        "Square Test",
    );
    assert.eq(
        25,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [0, 0],
                            [0, 2],
                            [2, 2],
                            [2, 0],
                        ],
                    },
                },
            })
            .count(),
        "Square Test 2",
    );

    if (1) {
        // SERVER-3726
        // Points exactly on diagonals may be in or out, depending on how the error calculating the
        // slope falls.
        assert.between(
            341 - 18,
            t
                .find({
                    loc: {
                        "$within": {
                            "$polygon": [
                                [0, 0],
                                [0, 10],
                                [10, 10],
                                [10, 0],
                                [5, 5],
                            ],
                        },
                    },
                })
                .count(),
            341,
            "Square Missing Chunk Test",
            true,
        );
        assert.between(
            21 - 2,
            t
                .find({
                    loc: {
                        "$within": {
                            "$polygon": [
                                [0, 0],
                                [0, 2],
                                [2, 2],
                                [2, 0],
                                [1, 1],
                            ],
                        },
                    },
                })
                .count(),
            21,
            "Square Missing Chunk Test 2",
            true,
        );
    }

    assert.eq(
        1,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [0, 0],
                            [0, 0],
                            [0, 0],
                        ],
                    },
                },
            })
            .count(),
        "Point Test",
    );

    // SERVER-3725
    {
        assert.eq(
            5,
            t
                .find({
                    loc: {
                        "$within": {
                            "$polygon": [
                                [0, 0],
                                [1, 0],
                                [2, 0],
                            ],
                        },
                    },
                })
                .count(),
            "Line Test 1",
        );
        assert.eq(
            3,
            t
                .find({
                    loc: {
                        "$within": {
                            "$polygon": [
                                [0, 0],
                                [0, 0],
                                [1, 0],
                            ],
                        },
                    },
                })
                .count(),
            "Line Test 2",
        );
        assert.eq(
            5,
            t
                .find({
                    loc: {
                        "$within": {
                            "$polygon": [
                                [0, 2],
                                [0, 1],
                                [0, 0],
                            ],
                        },
                    },
                })
                .count(),
            "Line Test 3",
        );
    }

    assert.eq(
        3,
        t
            .find({
                loc: {
                    "$within": {
                        "$polygon": [
                            [0, 1],
                            [0, 0],
                            [0, 0],
                        ],
                    },
                },
            })
            .count(),
        "Line Test 4",
    );
}
