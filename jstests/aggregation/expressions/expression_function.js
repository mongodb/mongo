/**
 * Tests basic functionality of the $function expression.
 *
 * @tags: [
 *   requires_scripting,
 *   # TODO SERVER-127997: The mozjs-wasm engine is incompatible with ASAN/TSAN: libwasmtime_engine.so
 *   # initialization takes >5 minutes under memory pressure, exceeding the 300s await_ready()
 *   # timeout, and the Wasmtime rayon thread pool triggers TSAN CHECK failures.
 *   mozjs_wasm_unsupported,
 * ]
 */
import {describe, it} from "jstests/libs/mochalite.js";
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db.expression_function;

function f_finalize(first, second) {
    return first + second;
}

describe("$function body forms", () => {
    it("accepts a function object as body", () => {
        coll.drop();
        for (let i = 0; i < 5; i++) assert.commandWorked(coll.insert({value: i}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        newValue: {
                            $function: {args: ["$value", -1], body: f_finalize, lang: "js"},
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert(
            resultsEq(results, [
                {newValue: -1},
                {newValue: 0},
                {newValue: 1},
                {newValue: 2},
                {newValue: 3},
            ]),
            results,
        );
    });

    it("accepts a stringified function as body", () => {
        coll.drop();
        for (let i = 0; i < 5; i++) assert.commandWorked(coll.insert({value: i}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        newValue: {
                            $function: {
                                args: ["$value", -1],
                                body: f_finalize.toString(),
                                lang: "js",
                            },
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert(
            resultsEq(results, [
                {newValue: -1},
                {newValue: 0},
                {newValue: 1},
                {newValue: 2},
                {newValue: 3},
            ]),
            results,
        );
    });

    it("accepts an expression that evaluates to an array for args", () => {
        coll.drop();
        for (let i = 0; i < 5; i++) assert.commandWorked(coll.insert({values: [i, i * 2]}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        newValue: {$function: {args: "$values", body: f_finalize, lang: "js"}},
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert(
            resultsEq(results, [
                {newValue: 0},
                {newValue: 3},
                {newValue: 6},
                {newValue: 9},
                {newValue: 12},
            ]),
            results,
        );
    });
});

describe("$function invalid arguments", () => {
    it("fails when args expression does not evaluate to an array", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            newValue: {
                                $function: {
                                    args: "must evaluate to an array",
                                    body: f_finalize,
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            31266,
        );
    });

    it("fails when body is an invalid function string", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            newValue: {
                                $function: {
                                    args: [1, 3],
                                    body: "this is not a valid function!",
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.JSInterpreterFailure,
        );
    });

    it("fails when lang is missing", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [{$project: {newValue: {$function: {args: [1, 3], body: f_finalize}}}}],
                cursor: {},
            }),
            31418,
        );
    });

    it("fails when lang is not 'js'", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            newValue: {
                                $function: {args: [1, 3], body: f_finalize, lang: "not js!"},
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            31419,
        );
    });

    it("fails when args is a plain string literal", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            newValue: {
                                $function: {args: "A string!", body: f_finalize, lang: "js"},
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            31266,
        );
    });
});

describe("$function argument handling", () => {
    it("missing field arrives as null/undefined", () => {
        coll.drop();
        assert.commandWorked(coll.insert({a: 5}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        result: {
                            $function: {
                                body: function (a, b) {
                                    return b === null || b === undefined ? a * 10 : a + b;
                                },
                                args: ["$a", "$nonexistent"],
                                lang: "js",
                            },
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert.eq(results[0].result, 50);
    });

    it("$$ROOT passes the entire document", () => {
        coll.drop();
        assert.commandWorked(coll.insert({x: 1, y: 2}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        keys: {
                            $function: {
                                body: function (doc) {
                                    return Object.keys(doc).sort();
                                },
                                args: ["$$ROOT"],
                                lang: "js",
                            },
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert.eq(results[0].keys, ["_id", "x", "y"]);
    });
});

describe("$function in aggregation stages", () => {
    it("works in $replaceWith", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 7}));
        const results = coll
            .aggregate([
                {
                    $replaceWith: {
                        $function: {
                            body: function (x) {
                                return {computed: x * 3};
                            },
                            args: ["$v"],
                            lang: "js",
                        },
                    },
                },
            ])
            .toArray();
        assert.eq(results, [{computed: 21}]);
    });
});

describe("$function error handling", () => {
    it("TypeError from bad property access propagates as JSInterpreterFailure", () => {
        coll.drop();
        assert.commandWorked(coll.insert({v: 1}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {
                        $project: {
                            v: {
                                $function: {
                                    body: function (x) {
                                        return x.nonexistent.property;
                                    },
                                    args: ["$v"],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.JSInterpreterFailure,
        );
    });
});

describe("$function BSON argument lifetime", () => {
    it("rejects retained argument access across invocations", () => {
        coll.drop();
        assert.commandWorked(coll.insert({_id: 1, x: "first"}));
        assert.commandWorked(coll.insert({_id: 2, x: "second"}));
        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: coll.getName(),
                pipeline: [
                    {$sort: {_id: 1}},
                    {
                        $project: {
                            result: {
                                $function: {
                                    body: function (arg) {
                                        if (typeof globalThis.savedArg === "undefined") {
                                            globalThis.savedArg = arg;
                                            return "saved";
                                        }
                                        return globalThis.savedArg.x;
                                    },
                                    args: ["$$ROOT"],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ],
                cursor: {},
            }),
            ErrorCodes.BadValue,
        );
    });
});

describe("$function BSON type handling", () => {
    it("Date argument supports method calls", () => {
        coll.drop();
        assert.commandWorked(coll.insert({d: new Date("2024-06-15")}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        year: {
                            $function: {
                                body: function (d) {
                                    return d.getFullYear();
                                },
                                args: ["$d"],
                                lang: "js",
                            },
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert.eq(results[0].year, 2024);
    });

    it("NumberLong argument supports arithmetic", () => {
        coll.drop();
        assert.commandWorked(coll.insert({n: NumberLong(1000000000)}));
        const results = coll
            .aggregate([
                {
                    $project: {
                        doubled: {
                            $function: {
                                body: function (n) {
                                    return n * 2;
                                },
                                args: ["$n"],
                                lang: "js",
                            },
                        },
                        _id: 0,
                    },
                },
            ])
            .toArray();
        assert.eq(results[0].doubled, 2000000000);
    });
});
