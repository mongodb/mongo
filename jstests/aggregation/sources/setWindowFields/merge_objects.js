/**
 * Test that $mergeObjects works as a window function.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

import {describe, it, beforeEach} from "jstests/libs/mochalite.js";

describe("$mergeObjects as a window function", function () {
    let coll;

    beforeEach(function () {
        coll = db[jsTestName()];
        coll.drop();
    });

    describe("Success cases", function () {
        it("should accumulate objects without lookahead", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1, obj: {b: 2}},
                    {_id: 2, obj: {c: 3}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: 1}, merged: {a: 1}}, results[0], results);
            assert.docEq({_id: 1, obj: {b: 2}, merged: {a: 1, b: 2}}, results[1], results);
            assert.docEq({_id: 2, obj: {c: 3}, merged: {a: 1, b: 2, c: 3}}, results[2], results);
        });

        it("should accumulate objects with lookahead", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1, obj: {b: 2}},
                    {_id: 2, obj: {c: 3}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", 1]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: 1}, merged: {a: 1, b: 2}}, results[0], results);
            assert.docEq({_id: 1, obj: {b: 2}, merged: {a: 1, b: 2, c: 3}}, results[1], results);
            assert.docEq({_id: 2, obj: {c: 3}, merged: {a: 1, b: 2, c: 3}}, results[2], results);
        });

        it("should handle overlapping fields with later documents overriding values", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1, b: 10}},
                    {_id: 1, obj: {a: 2, c: 20}},
                    {_id: 2, obj: {a: 3, d: 30}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: 1, b: 10}, merged: {a: 1, b: 10}}, results[0], results);
            assert.docEq({_id: 1, obj: {a: 2, c: 20}, merged: {a: 2, b: 10, c: 20}}, results[1], results);
            assert.docEq({_id: 2, obj: {a: 3, d: 30}, merged: {a: 3, b: 10, c: 20, d: 30}}, results[2], results);
        });

        it("should handle empty documents in window", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1, obj: {}},
                    {_id: 2, obj: {b: 2}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: 1}, merged: {a: 1}}, results[0], results);
            assert.docEq({_id: 1, obj: {}, merged: {a: 1}}, results[1], results);
            assert.docEq({_id: 2, obj: {b: 2}, merged: {a: 1, b: 2}}, results[2], results);
        });

        it("should work with multiple partitions", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, key: 1, obj: {a: 1}},
                    {_id: 1, key: 1, obj: {b: 2}},
                    {_id: 2, key: 2, obj: {c: 3}},
                    {_id: 3, key: 2, obj: {d: 4}},
                    {_id: 4, key: 3, obj: {e: 5}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            partitionBy: "$key",
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(5, results.length, results);
            assert.docEq({_id: 0, key: 1, obj: {a: 1}, merged: {a: 1}}, results[0], results);
            assert.docEq({_id: 1, key: 1, obj: {b: 2}, merged: {a: 1, b: 2}}, results[1], results);
            assert.docEq({_id: 2, key: 2, obj: {c: 3}, merged: {c: 3}}, results[2], results);
            assert.docEq({_id: 3, key: 2, obj: {d: 4}, merged: {c: 3, d: 4}}, results[3], results);
            assert.docEq({_id: 4, key: 3, obj: {e: 5}, merged: {e: 5}}, results[4], results);
        });

        it("should replace nested documents entirely rather than merging recursively", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: {x: 1}}},
                    {_id: 1, obj: {a: {y: 2}}},
                    {_id: 2, obj: {b: 3}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: {x: 1}}, merged: {a: {x: 1}}}, results[0], results);
            assert.docEq({_id: 1, obj: {a: {y: 2}}, merged: {a: {y: 2}}}, results[1], results);
            assert.docEq({_id: 2, obj: {b: 3}, merged: {a: {y: 2}, b: 3}}, results[2], results);
        });

        it("should handle unbounded range windows with the same merged result for all documents", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1, obj: {b: 2}},
                    {_id: 2, obj: {c: 3}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {range: ["unbounded", "unbounded"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);

            const expectedMerged = {a: 1, b: 2, c: 3};
            assert.docEq({_id: 0, obj: {a: 1}, merged: expectedMerged}, results[0], results);
            assert.docEq({_id: 1, obj: {b: 2}, merged: expectedMerged}, results[1], results);
            assert.docEq({_id: 2, obj: {c: 3}, merged: expectedMerged}, results[2], results);
        });

        it("should handle missing and null fields", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1}, // missing obj field
                    {_id: 2, obj: null}, // null obj field
                    {_id: 3, obj: {b: 2}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(4, results.length, results);
            assert.docEq({_id: 0, obj: {a: 1}, merged: {a: 1}}, results[0], results);
            assert.docEq({_id: 1, merged: {a: 1}}, results[1], results);
            assert.docEq({_id: 2, obj: null, merged: {a: 1}}, results[2], results);
            assert.docEq({_id: 3, obj: {b: 2}, merged: {a: 1, b: 2}}, results[3], results);
        });

        it("should handle conflicting complex nested structures by replacing them entirely", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: [1, 2], b: {x: 1}}},
                    {_id: 1, obj: {a: [3, 4], b: {y: 2}}},
                    {_id: 2, obj: {c: "test"}},
                ]),
            );

            const results = coll
                .aggregate([
                    {
                        $setWindowFields: {
                            sortBy: {_id: 1},
                            output: {merged: {$mergeObjects: "$obj", window: {documents: ["unbounded", "current"]}}},
                        },
                    },
                ])
                .toArray();

            assert.eq(3, results.length, results);
            assert.docEq({_id: 0, obj: {a: [1, 2], b: {x: 1}}, merged: {a: [1, 2], b: {x: 1}}}, results[0], results);
            assert.docEq({_id: 1, obj: {a: [3, 4], b: {y: 2}}, merged: {a: [3, 4], b: {y: 2}}}, results[1], results);
            assert.docEq({_id: 2, obj: {c: "test"}, merged: {a: [3, 4], b: {y: 2}, c: "test"}}, results[2], results);
        });
    });

    describe("Failure cases", function () {
        it("should not support removable windows", function () {
            assert.commandWorked(
                coll.insertMany([
                    {_id: 0, obj: {a: 1}},
                    {_id: 1, obj: {b: 2}},
                    {_id: 2, obj: {c: 3}},
                ]),
            );

            assert.throwsWithCode(
                () =>
                    coll
                        .aggregate([
                            {
                                $setWindowFields: {
                                    sortBy: {_id: 1},
                                    output: {merged: {$mergeObjects: "$obj", window: {documents: [-1, "current"]}}},
                                },
                            },
                        ])
                        .toArray(),
                5461500,
            );
        });
    });
});
