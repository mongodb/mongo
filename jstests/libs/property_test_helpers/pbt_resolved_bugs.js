/*
 * This file contains a list of bugs we've found from property-based testing. We use this list to
 * run the scenarios at the beginning of testing to make sure they don't fail anymore.
 *
 * Some of these scenarios are hard to find by chance. When a change is made to a PBT model, we
 * may not run into these scenarios anymore since stability of the generated examples is not
 * guaranteed. As a solution we usually write a new non-PBT test with the example to make sure we
 * have coverage, but we can also run them through existing PBTs using this file.
 *
 * This file is also useful for BFs. fast-check will print out the counterexample, and it can be
 * pasted into this file to repro the bug on the first run. After that further analysis can be done.
 */

// Repro from SERVER-102825.
const partialIndexExample102825 = {
    collSpec: {
        isTS: false,
        docs: [{_id: 0, a: 0}],
        indexes: [
            {
                def: {a: 1},
                options: {partialFilterExpression: {$or: [{a: 1}, {a: {$lte: "a string"}}]}},
            },
        ],
    },
    queries: [
        [{$match: {$or: [{a: 1}, {a: {$lte: "a string"}}], _id: {$lte: 5}}}],
        [{$match: {$or: [{a: 1}, {a: {$lte: 10}}], _id: {$lte: 5}}}],
    ],
};

const partialIndexExample2_partialFilter = {
    $or: [{a: {$lt: ""}}, {_id: {$eq: 0}, a: {$eq: 0}}],
};
const partialIndexExample2 = {
    collSpec: {
        isTS: false,
        docs: [{_id: 1, m: 0, a: 0, b: 0}],
        indexes: [
            {def: {a: 1}, options: {partialFilterExpression: partialIndexExample2_partialFilter}},
            {
                def: {a: 1, m: 1},
                options: {partialFilterExpression: partialIndexExample2_partialFilter},
            },
            {
                def: {b: 1, a: 1},
                options: {partialFilterExpression: partialIndexExample2_partialFilter},
            },
        ],
    },
    queries: [
        [{$match: {$or: [{a: {$lt: ""}}, {_id: {$eq: 0}, a: {$eq: -1}}]}}, {$sort: {b: 1}}],
        [{$match: {$or: [{a: {$lt: 1}}, {_id: {$eq: 0}, a: {$eq: 0}}]}}, {$sort: {b: 1}}],
    ],
};

export const partialIndexCounterexamples = [
    // I'm not sure why examples have to be placed in an array, the documentation doesn't say. I've
    // verified it works though.
    [partialIndexExample102825],
    [partialIndexExample2],
];

// Repro from SERVER-106983.
const planStabilityExample106983 = {
    collSpec: {
        isTS: false,
        docs: [
            {_id: 187, t: ISODate("1970-01-01T00:00:00Z"), array: [], a: 0},
            {_id: 83, t: ISODate("1970-01-01T00:00:00Z"), array: "", a: 0},
        ],
        indexes: [{def: {t: 1, array: 1}, options: {}}],
    },
    queries: [
        [
            {
                $match: {
                    $or: [{t: {$exists: true}}, {_id: 0, a: 0}],
                    $and: [{array: {$nin: [0]}}, {array: {$eq: ""}}],
                },
            },
        ],
    ],
};

export const planStabilityCounterexamples = [[planStabilityExample106983]];
