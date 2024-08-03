/**
 * Test $unwind with 'includeArrayIndex'.
 */
import {show} from "jstests/libs/golden_test.js";

const coll = db.unwind;
coll.drop();

let docs = [
    {_id: 1, a: [{b: {d: 3}, p: 4}, {b: {d: 5}}, 6], z: 7},
    {_id: 2, a: [{b: {c: 3, d: 4}, p: 5}, {b: {c: {d: 6}, d: 7}, p: 8}, 9], z: 10},
    {_id: 3, a: [{b: {c: [3, {d: 4}], d: 5}, p: 6}, {b: {c: [7, {d: 8}], d: 9}, p: 10}], z: 11},
    {_id: 4, a: [{b: [{d: 3}, {d: 4}, 5], p: 6}, {b: [{d: 7}, {d: 8}, 9], p: 10}], z: 11},
    {
        _id: 5,
        a: [
            {b: [{c: 3, d: 4}, {c: 5, d: 6}, 7], p: 8},
            {b: [{c: 9, d: 10}, {c: 11, d: 12}, 13], p: 14}
        ],
        z: 15
    },
    {
        _id: 6,
        a: [
            {b: [{c: [3, {d: 4}], d: 5}, {c: [6, {d: 7}], d: 8}, 9], p: 10},
            {b: [{c: [11, {d: 12}], d: 13}, {c: [14, {d: 15}], d: 16}, 17], p: 18}
        ],
        z: 19
    },
    {_id: 7, a: [{b: {d: 3}}, {b: {d: 4}}, 5], j: {p: 7}, z: 8},
    {_id: 8, a: [{b: {d: 3}}, {b: {d: 4}}, 5], j: {k: 7, p: 8}, z: 9},
    {_id: 9, a: [{b: {d: 3}}, {b: {d: 4}}, 5], j: [], z: 7},
    {_id: 10, a: [{b: {d: 3}}, {b: {d: 4}}, 5], j: [{k: 7, p: 8}, {k: 9, p: 10}, 11], z: 7},
    {_id: 11, a: [{b: {d: 3}}, {b: {d: 4}}, 5], j: {k: [7, 8], p: 9}, z: 10},
    {
        _id: 12,
        a: [{b: {d: 3}}, {b: {d: 4}}, 5],
        j: [{k: [7, 8], p: 9}, {k: [10, 11], p: 12}, 13],
        z: 14
    },
    {_id: 13, a: {b: [{c: {e: 3}, p: 4}, {c: {e: 5}}, 6], y: 7}, z: 7},
    {_id: 14, a: {b: [{c: {d: 3, e: 4}, p: 5}, {c: {d: {e: 6}, e: 7}, p: 8}, 9], y: 10}, z: 11},
    {
        _id: 15,
        a: {b: [{c: {d: [3, {e: 4}], e: 5}, p: 6}, {c: {d: [7, {e: 8}], e: 9}, p: 10}], y: 11},
        z: 12
    },
    {
        _id: 16,
        a: {b: [{c: [{e: 3}, {e: 4}, 5], p: 6}, {c: [{e: 7}, {e: 8}, 9], p: 10}], y: 11},
        z: 12
    },
    {
        _id: 17,
        a: {
            b: [
                {c: [{d: 3, e: 4}, {d: 5, e: 6}, 7], p: 8},
                {c: [{d: 9, e: 10}, {d: 11, e: 12}, 13], p: 14}
            ],
            y: 15
        },
        z: 16
    },
    {
        _id: 18,
        a: {
            b: [
                {c: [{d: [3, {e: 4}], e: 5}, {d: [6, {e: 7}], e: 8}, 9], p: 10},
                {c: [{d: [11, {e: 12}], e: 13}, {d: [14, {e: 15}], e: 16}, 17], p: 18}
            ],
            y: 19
        },
        z: 20
    },
    {_id: 19, a: {b: [{c: {e: 3}, p: 4}, {c: {e: 5}}, 6], y: 7}, j: {p: 8}, z: 9},
    {_id: 20, a: {b: [{c: {e: 3}, p: 4}, {c: {e: 5}}, 6], y: 7}, j: {k: 8, p: 9}, z: 10},
    {_id: 21, a: {b: [{c: {e: 3}, p: 4}, {c: {e: 5}}, 6], y: 7}, j: [], z: 7},
    {
        _id: 22,
        a: {b: [{c: {e: 3}, p: 4}, {c: {e: 5}}, 6], y: 7},
        j: [{k: 7, p: 8}, {k: 9, p: 10}, 11],
        z: 7
    },
    {_id: 23, a: {b: [{c: {e: 3}}, {c: {e: 4}}, 5], y: 6}, j: {k: [7, 8], p: 9}, z: 10},
    {
        _id: 24,
        a: {b: [{c: {e: 3}}, {c: {e: 4}}, 5], y: 6},
        j: [{k: [7, 8], p: 9}, {k: [10, 11], p: 12}, 13],
        z: 14
    }
];

let testcases = [
    [{$match: {_id: 1}}, {$unwind: {path: "$a"}}],
    [{$match: {_id: 1}}, {$unwind: {path: "$a", includeArrayIndex: "j"}}],
    [{$match: {_id: 7}}, {$unwind: {path: "$a", includeArrayIndex: "j"}}],
    [{$match: {_id: 21}}, {$unwind: {path: "$a", includeArrayIndex: "j"}}],
    [{$match: {_id: 22}}, {$unwind: {path: "$a", includeArrayIndex: "j"}}],
    [{$match: {_id: 1}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 7}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 8}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 9}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 10}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 11}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 12}}, {$unwind: {path: "$a", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 1}}, {$unwind: {path: "$a", includeArrayIndex: "a"}}],
    [{$match: {_id: 1}}, {$unwind: {path: "$a", includeArrayIndex: "a.b"}}],
    [{$match: {_id: 4}}, {$unwind: {path: "$a", includeArrayIndex: "a.b"}}],
    [{$match: {_id: 1}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 2}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 3}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 4}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 5}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 6}}, {$unwind: {path: "$a", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b", includeArrayIndex: "j"}}],
    [{$match: {_id: 19}}, {$unwind: {path: "$a.b", includeArrayIndex: "j"}}],
    [{$match: {_id: 21}}, {$unwind: {path: "$a.b", includeArrayIndex: "j"}}],
    [{$match: {_id: 22}}, {$unwind: {path: "$a.b", includeArrayIndex: "j"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 19}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 20}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 21}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 22}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 23}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 24}}, {$unwind: {path: "$a.b", includeArrayIndex: "j.k"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 16}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c"}}],
    [{$match: {_id: 13}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}],
    [{$match: {_id: 14}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}],
    [{$match: {_id: 15}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}],
    [{$match: {_id: 16}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}],
    [{$match: {_id: 17}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}],
    [{$match: {_id: 18}}, {$unwind: {path: "$a.b", includeArrayIndex: "a.b.c.d"}}]
];

coll.insert(docs);

for (let i = 0; i < testcases.length; ++i) {
    let pipeline = testcases[i].concat({$project: {x: 0}});

    print(`Query ${i}: ${tojsononeline(pipeline)}\n`);

    show(coll.aggregate(pipeline));

    print("\n");
}
