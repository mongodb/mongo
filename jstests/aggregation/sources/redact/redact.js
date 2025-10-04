// Check $redact pipeline stage.
import {anyEq} from "jstests/aggregation/extras/utils.js";

let t = db.jstests_aggregation_redact;
t.drop();

// this document will always be present but its content will change
t.save({
    _id: 1,
    level: 1,
    // b will present on level 3, 4, and 5
    b: {
        level: 3,
        c: 5, // always included when b is included
        // the contents of d test that if we cannot see a document then we cannot see its
        // array-nested subdocument even if we have permissions to see the subdocument.
        // it also tests arrays containing documents we cannot see
        d: [
            {level: 1, e: 4},
            {f: 6},
            {level: 5, g: 9},
            "NOT AN OBJECT!!11!", // always included when b is included
            [2, 3, 4, {level: 1, r: 11}, {level: 5, s: 99}],
            // nested array should always be included once b is
            // but the second object should only show up at level 5
        ],
    },
    // the contents of h test that in order to see a subdocument (j) we must be able to see all
    // parent documents (h and i) even if we have permissions to see the subdocument
    h: {level: 2, i: {level: 4, j: {level: 1, k: 8}}},
    // l checks that we get an empty document when we can see a document but none of its fields
    l: {m: {level: 3, n: 12}},
    // o checks that we get an empty array when we can see a array but none of its entries
    o: [{level: 5, p: 19}],
    // q is a basic field check and should always be included
    q: 14,
});

// this document will sometimes be missing
t.save({
    _id: 2,
    level: 4,
});

let a1 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 1]}, "$$DESCEND", "$$PRUNE"]}});
let a2 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 2]}, "$$DESCEND", "$$PRUNE"]}});
let a3 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 3]}, "$$DESCEND", "$$PRUNE"]}});
let a4 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 4]}, "$$DESCEND", "$$PRUNE"]}});
let a5 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 5]}, "$$DESCEND", "$$PRUNE"]}});

let a1result = [{_id: 1, level: 1, l: {}, o: [], q: 14}];

let a2result = [
    {
        _id: 1,
        level: 1,
        h: {
            level: 2,
        },
        l: {},
        o: [],
        q: 14,
    },
];

let a3result = [
    {
        _id: 1,
        level: 1,
        b: {
            level: 3,
            c: 5,
            d: [{level: 1, e: 4}, {f: 6}, "NOT AN OBJECT!!11!", [2, 3, 4, {level: 1, r: 11}]],
        },
        h: {
            level: 2,
        },
        l: {m: {level: 3, n: 12}},
        o: [],
        q: 14,
    },
];

let a4result = [
    {
        _id: 1,
        level: 1,
        b: {
            level: 3,
            c: 5,
            d: [{level: 1, e: 4}, {f: 6}, "NOT AN OBJECT!!11!", [2, 3, 4, {level: 1, r: 11}]],
        },
        h: {level: 2, i: {level: 4, j: {level: 1, k: 8}}},
        l: {m: {level: 3, n: 12}},
        o: [],
        q: 14,
    },
    {
        _id: 2,
        level: 4,
    },
];

let a5result = [
    {
        _id: 1,
        level: 1,
        b: {
            level: 3,
            c: 5,
            d: [
                {level: 1, e: 4},
                {f: 6},
                {level: 5, g: 9},
                "NOT AN OBJECT!!11!",
                [2, 3, 4, {level: 1, r: 11}, {level: 5, s: 99}],
            ],
        },
        h: {level: 2, i: {level: 4, j: {level: 1, k: 8}}},
        l: {m: {level: 3, n: 12}},
        o: [{level: 5, p: 19}],
        q: 14,
    },
    {
        _id: 2,
        level: 4,
    },
];

assert(anyEq(a1.toArray(), a1result));
assert(anyEq(a2.toArray(), a2result));
assert(anyEq(a3.toArray(), a3result));
assert(anyEq(a4.toArray(), a4result));
assert(anyEq(a5.toArray(), a5result));

// Test redacts that are just a variable access (this can happen as a result of optimizations)
assert.eq(t.aggregate({$redact: "$$PRUNE"}).toArray(), []);
assert(anyEq(t.aggregate({$redact: "$$KEEP"}).toArray(), t.find().toArray()));
assert(anyEq(t.aggregate({$redact: "$$DESCEND"}).toArray(), t.find().toArray()));

// test $$KEEP
t.drop();
// entire document should be present at 2 and beyond
t.save({_id: 1, level: 2, b: {level: 3, c: 2}, d: {level: 1, e: 8}, f: 9});

let b1 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 1]}, "$$KEEP", "$$PRUNE"]}});
let b2 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 2]}, "$$KEEP", "$$PRUNE"]}});
let b3 = t.aggregate({$redact: {$cond: [{$lte: ["$level", 3]}, "$$KEEP", "$$PRUNE"]}});

let b1result = [];

let b23result = [{_id: 1, level: 2, b: {level: 3, c: 2}, d: {level: 1, e: 8}, f: 9}];

assert(anyEq(b1.toArray(), b1result));
assert(anyEq(b2.toArray(), b23result));
assert(anyEq(b3.toArray(), b23result));
