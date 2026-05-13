// Test the behavior of user-defined (Javascript) accumulators.
//
// @tags: [
//   requires_scripting,
// ]
import {resultsEq} from "jstests/aggregation/extras/utils.js";

db.accumulator_js.drop();

for (const word of ["hello", "world", "world", "hello", "hi"]) {
    db.accumulator_js.insert({word: word, val: 1});
}

const command = {
    aggregate: "accumulator_js",
    cursor: {},
    pipeline: [
        {
            $group: {
                _id: "$word",
                wordCount: {
                    $accumulator: {
                        init: function () {
                            return 0;
                        },
                        accumulateArgs: ["$val"],
                        accumulate: function (state, val) {
                            return state + val;
                        },
                        merge: function (state1, state2) {
                            return state1 + state2;
                        },
                        finalize: function (state) {
                            return state;
                        },
                    },
                },
            },
        },
    ],
};

let expectedResults = [
    {_id: "hello", wordCount: 2},
    {_id: "world", wordCount: 2},
    {_id: "hi", wordCount: 1},
];

let res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that the functions can be passed as strings.
{
    const accumulatorSpec = command.pipeline[0].$group.wordCount.$accumulator;
    accumulatorSpec.init = accumulatorSpec.init.toString();
    accumulatorSpec.accumulate = accumulatorSpec.accumulate.toString();
    accumulatorSpec.merge = accumulatorSpec.merge.toString();
    accumulatorSpec.finalize = accumulatorSpec.finalize.toString();
}
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that finalize is optional.
delete command.pipeline[0].$group.wordCount.$accumulator.finalize;
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test a finalizer other than the identity function. Finalizers are useful when the intermediate
// state needs to be a different format from the final result.
res = assert.commandWorked(
    db.runCommand(
        Object.merge(command, {
            pipeline: [
                {
                    $group: {
                        _id: 1,
                        avgWordLen: {
                            $accumulator: {
                                init: function () {
                                    return {count: 0, sum: 0};
                                },
                                accumulateArgs: [{$strLenCP: "$word"}],
                                accumulate: function ({count, sum}, wordLen) {
                                    return {count: count + 1, sum: sum + wordLen};
                                },
                                merge: function (s1, s2) {
                                    return {count: s1.count + s2.count, sum: s1.sum + s2.sum};
                                },
                                finalize: function ({count, sum}) {
                                    return sum / count;
                                },
                                lang: "js",
                            },
                        },
                    },
                },
            ],
        }),
    ),
);
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, avgWordLen: 22 / 5}]), res.cursor);

// Test that a null word is considered a valid value.
assert.commandWorked(db.accumulator_js.insert({word: null, val: 1}));
expectedResults.push({_id: null, wordCount: 1});
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that missing fields become JS null.
// This is similar to how most other agg operators work.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert({sentinel: 1}));
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {
                        return [];
                    },
                    accumulateArgs: ["$no_such_field"],
                    accumulate: function (state, value) {
                        return state.concat([value]);
                    },
                    merge: function (s1, s2) {
                        return s1.concat(s2);
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, value: [null]}]), res.cursor);

// Test that initArgs must evaluate to an array.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {},
                    initArgs: {$const: 5},
                    accumulateArgs: [],
                    accumulate: function () {},
                    merge: function () {},
                    lang: "js",
                },
            },
        },
    },
];
assert.commandFailedWithCode(db.runCommand(command), 4544711);

// Test that initArgs is passed to init.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function (str1, str2) {
                        return "initial_state_set_from_" + str1 + "_and_" + str2;
                    },
                    initArgs: ["ABC", "DEF"],
                    accumulateArgs: [],
                    accumulate: function (state) {
                        return state;
                    },
                    merge: function (s1, s2) {
                        return s1;
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, value: "initial_state_set_from_ABC_and_DEF"}]), res.cursor);

// Test that when initArgs errors, we fail gracefully, and don't call init.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {
                        throw "init should not be called";
                    },
                    // Use $cond to thwart constant folding, to ensure we are testing evaluate rather
                    // than optimize.
                    initArgs: {$add: {$cond: ["$foo", "", ""]}},
                    accumulateArgs: [],
                    accumulate: function () {
                        throw "accumulate should not be called";
                    },
                    merge: function () {
                        throw "merge should not be called";
                    },
                    lang: "js",
                },
            },
        },
    },
];
// ErrorCodes.TypeMismatch means "$add only supports numeric or date types". Code 16554 represented
// a type mismatch before 6.1 for this specific check.
assert.commandFailedWithCode(db.runCommand(command), [16554, ErrorCodes.TypeMismatch]);

// Test that initArgs can have a different length per group.
assert(db.accumulator_js.drop());
assert.commandWorked(
    db.accumulator_js.insert([
        {_id: 1, a: ["A", "B", "C"]},
        {_id: 2, a: ["A", "B", "C"]},
        {_id: 3, a: ["X", "Y"]},
        {_id: 4, a: ["X", "Y"]},
    ]),
);
command.pipeline = [
    {
        $group: {
            _id: {a: "$a"},
            value: {
                $accumulator: {
                    init: function (...args) {
                        return args.toString();
                    },
                    initArgs: "$a",
                    accumulateArgs: [],
                    accumulate: function (state) {
                        return state;
                    },
                    merge: function (s1, s2) {
                        return s1;
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
assert(
    resultsEq(res.cursor.firstBatch, [
        {_id: {a: ["A", "B", "C"]}, value: "A,B,C"},
        {_id: {a: ["X", "Y"]}, value: "X,Y"},
    ]),
    res.cursor,
);

// Test that accumulateArgs must evaluate to an array.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {},
                    accumulateArgs: {$const: 5},
                    accumulate: function (state, value) {},
                    merge: function (s1, s2) {},
                    lang: "js",
                },
            },
        },
    },
];
assert.commandFailedWithCode(db.runCommand(command), 4544712);

// Test that accumulateArgs can have more than one element.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {},
                    accumulateArgs: ["ABC", "DEF"],
                    accumulate: function (state, str1, str2) {
                        return str1 + str2;
                    },
                    merge: function (s1, s2) {
                        return s1 || s2;
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
expectedResults = [{_id: 1, value: "ABCDEF"}];
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that accumulateArgs can have a different length per document.
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {
                        return [];
                    },
                    accumulateArgs: "$a",
                    accumulate: function (state, ...values) {
                        state.push(values);
                        state.sort();
                        return state;
                    },
                    merge: function (s1, s2) {
                        return s1.concat(s2);
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
expectedResults = [
    {
        _id: 1,
        value: [
            ["A", "B", "C"],
            ["A", "B", "C"],
            ["X", "Y"],
            ["X", "Y"],
        ],
    },
];
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that accumulateArgs can contain expressions that evaluate to null or missing.
// The behavior is the same as ExpressionArray: arrays can't contain missing, so any expressions
// that evaluate to missing get converted to null. Then, the nulls get serialized to BSON and passed
// to JS as usual.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert({}));
command.pipeline = [
    {
        $group: {
            _id: 1,
            value: {
                $accumulator: {
                    init: function () {
                        return null;
                    },
                    accumulateArgs: [
                        null,
                        "$no_such_field",
                        {$let: {vars: {not_an_object: 5}, in: "$not_an_object.field"}},
                    ],
                    accumulate: function (state, ...values) {
                        return {
                            len: values.length,
                            types: values.map((v) => typeof v),
                            values: values,
                        };
                    },
                    merge: function (s1, s2) {
                        return s1 || s2;
                    },
                    lang: "js",
                },
            },
        },
    },
];
res = assert.commandWorked(db.runCommand(command));
expectedResults = [{_id: 1, value: {len: 3, types: ["object", "object", "object"], values: [null, null, null]}}];
assert(resultsEq(res.cursor.firstBatch, expectedResults), res.cursor);

// Test that throwing inside accumulate causes the command to fail.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert({val: 1}));
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: "accumulator_js",
        cursor: {},
        pipeline: [
            {
                $group: {
                    _id: 1,
                    value: {
                        $accumulator: {
                            init: function () {
                                return 0;
                            },
                            accumulateArgs: ["$val"],
                            accumulate: function (state, val) {
                                throw new Error("accumulate error");
                            },
                            merge: function (s1, s2) {
                                return s1 + s2;
                            },
                            lang: "js",
                        },
                    },
                },
            },
        ],
    }),
    ErrorCodes.JSInterpreterFailure,
);

// Test that init returning null is handled: accumulate receives null as initial state.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert([{val: 10}, {val: 32}]));
res = assert.commandWorked(
    db.runCommand({
        aggregate: "accumulator_js",
        cursor: {},
        pipeline: [
            {
                $group: {
                    _id: 1,
                    value: {
                        $accumulator: {
                            init: function () {
                                return null;
                            },
                            accumulateArgs: ["$val"],
                            accumulate: function (state, val) {
                                return (state || 0) + val;
                            },
                            merge: function (s1, s2) {
                                return (s1 || 0) + (s2 || 0);
                            },
                            lang: "js",
                        },
                    },
                },
            },
        ],
    }),
);
assert(resultsEq(res.cursor.firstBatch, [{_id: 1, value: 42}]), res.cursor);

// Test that string state works: accumulate builds a comma-separated string.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert([{word: "hello"}, {word: "world"}]));
res = assert.commandWorked(
    db.runCommand({
        aggregate: "accumulator_js",
        cursor: {},
        pipeline: [
            {
                $sort: {word: 1},
            },
            {
                $group: {
                    _id: 1,
                    value: {
                        $accumulator: {
                            init: function () {
                                return "";
                            },
                            accumulateArgs: ["$word"],
                            accumulate: function (state, val) {
                                return state ? state + "," + val : val;
                            },
                            merge: function (s1, s2) {
                                return s1 ? s1 + "," + s2 : s2;
                            },
                            finalize: function (state) {
                                return state;
                            },
                            lang: "js",
                        },
                    },
                },
            },
        ],
    }),
);
assert.eq(res.cursor.firstBatch.length, 1);
// The two words appear in the result separated by a comma (order may vary across shards).
const words = res.cursor.firstBatch[0].value.split(",").sort();
assert.sameMembers(words, ["hello", "world"]);

// Test that when no documents match a group, finalize is called on the init state.
assert(db.accumulator_js.drop());
assert.commandWorked(db.accumulator_js.insert([{x: 1}, {x: 1}]));
res = assert.commandWorked(
    db.runCommand({
        aggregate: "accumulator_js",
        cursor: {},
        pipeline: [
            {$match: {x: 999}},
            {
                $group: {
                    _id: 1,
                    value: {
                        $accumulator: {
                            init: function () {
                                return {count: 0, sum: 0};
                            },
                            accumulateArgs: ["$x"],
                            accumulate: function (state, val) {
                                return {count: state.count + 1, sum: state.sum + val};
                            },
                            merge: function (s1, s2) {
                                return {count: s1.count + s2.count, sum: s1.sum + s2.sum};
                            },
                            finalize: function (state) {
                                return state.count > 0 ? state.sum / state.count : 0;
                            },
                            lang: "js",
                        },
                    },
                },
            },
        ],
    }),
);
// No documents matched, so the group produces no output.
assert.eq(res.cursor.firstBatch.length, 0, res.cursor);
