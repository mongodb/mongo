load("jstests/query_golden/libs/utils.js");

/**
 * Drops 'coll' and re-populates it according to 'docs' and 'indexes'. Then, runs the specified
 * projections against the collection and prints the results.
 */
function runProjectionsAgainstColl(coll, docs, indexes, projSpecs) {
    resetCollection(coll, docs, indexes);

    for (const projectionSpec of projSpecs) {
        const pipeline = [{$project: projectionSpec}];
        jsTestLog(`Query: ${tojsononeline(pipeline)}`);
        show(coll.aggregate(pipeline));

        // TODO SERVER-71071: Consider adding a loop here which runs the aggregation and hints
        // each of the provided indexes.
    }
}

/**
 * Returns some example docs with interesting values as paths "a", "a.b", and "a.b.c".
 */
function getProjectionDocs() {
    return [
        //
        // Simple documents without any arrays along "a.b.c".
        //

        // "a" is missing/null.
        {},
        {a: null},
        {a: undefined},
        {x: "str", y: "str"},

        // "a.b" is missing/null.
        {a: "str", b: "str", x: "str", y: "str"},
        {a: {}},
        {a: {b: null, c: 1}},
        {a: {b: undefined, c: 1}},
        {a: {c: 1, d: 1}},
        {a: {d: 1}},
        {a: {c: {b: 1}}},

        // "a.b.c" is missing/null
        {a: {b: 1, c: 1, d: 1}, x: {y: 1, z: 1}},
        {a: {b: 1, _id: 1}},
        {a: {b: {}}},
        {a: {b: {c: null}}},
        {a: {b: {c: undefined}}},
        {a: {b: {x: 1, y: 1}}},

        // All path components along "a.b.c" exist.
        {a: {b: {c: "str", d: "str", e: "str"}, f: "str"}, x: "str"},

        // Fields with similar names to the projected fields.
        {"a.b.c": 1},
        {a: "str", abc: "str", "a.b": "str"},
        {a: {b: 1}, "a.b": "str"},
        {a: {bNot: {c: 1}}},

        //
        // Documents with arrays on the "a.b.c" path.
        //

        // "a.b" is missing/null
        {a: [], x: "str"},
        {a: ["str"]},
        {a: [null]},
        {a: [null, null]},
        {a: [null, "str"]},
        {a: [[], []]},
        {a: [[1, 2]]},
        {a: [{}, {}]},
        {a: [{x: "str"}]},
        {a: [{c: "str"}]},
        {a: [{b: null, c: 1}, {c: 1}, {d: 1}, "str"]},

        // "a.b.c" is missing/null
        {a: {b: [], x: "str"}},
        {a: {b: ["str"]}},
        {a: {b: [[]]}},
        {a: {b: [{}]}},
        {a: {b: [{c: null}]}},
        {a: [{b: {x: 1}}]},
        {a: [{b: [{}]}]},
        {a: [[], [[], [], [1], [{c: 1}]], {b: 1}]},

        // Fields with similar names to the projected fields.
        {
            a: [
                // No "a.b".
                {bNot: [{c: "str"}, {c: "str"}]},
                // No "a.b.c".
                {b: [{cNot: "str", d: 1}, {cNot: "str", d: 2}]},
                // Only some "a.b.c"s.
                {b: [{c: 3, d: 3}, {cNot: "str", d: 4}, {c: 5}]},
            ]
        },

        //
        // All path components along "a.b.c" exist.
        //

        // Exactly one array along the "a.b.c" path.
        {a: [{b: {c: 2, d: 3}, e: 4}, {b: {c: 5, d: 6}, e: 7}]},
        {a: {b: [{c: 1, d: 1}, {c: 2, d: 2}]}},
        {a: {b: {c: [1, 2, {d: 3}]}}},
        {a: ["str", {b: 1}, {c: 1}, {b: 1, c: 1, d: 1}], x: "str"},

        // Two arrays along the "a.b.c" path.
        {a: [{b: [{c: 1, d: 1}, {c: 2, d: 2}]}, {b: [{c: 3, d: 3}, {c: 4, d: 4}]}]},
        {a: [{b: {c: [1, {d: 1}]}}, {b: {c: []}}]},
        {a: {b: [{c: [1, {d: 1}]}, {c: []}]}},

        // "a", "a.b", and "a.b.c" are arrays.
        {
            a: [
                {b: [{c: [1, 2, 3], d: 1}, {c: [2, 3, 4], d: 2}]},
                {b: [{c: [3, 4, 5], d: 3}, {c: [4, 5, 6], d: 4}]}
            ]
        },

        // Multiple nested arrays encountered between field path components.
        {a: [[1, {b: 1}, {b: 2, c: 2}, "str"]]},
        {a: [[[{b: [[[{c: [[["str"]]], d: "str"}]]]}]]]},
        {
            a: [
                ["str", {b: 1}, {b: 2, c: 2}, "str"],
                [[{b: 1}]],
                [{b: 1}, [{b: 2}], [[{b: [2]}]]],
            ]
        },

    ];
}

/**
 * Similar to getProjectionDocs(), but a smaller list where the interesting values are just under
 * the _id field.
 */
function getIdProjectionDocs() {
    return [
        {_id: 1, x: 2},
        {_id: {}, x: 1},
        {_id: {x: 1}, y: 2},
        {_id: {a: 1, c: 2}, x: 3},
        {_id: {a: 1, b: 2, c: 3}, x: 4},
        {_id: {a: {b: {c: 1}, d: 2}, e: 3}, x: 4},
        {_id: {a: [[{b: {c: 1}, d: 2}, 3], 4], e: 5}, x: 6},
    ];
}
