/**
 * @tags: [
 *   requires_scripting,
 *   requires_fcv_83,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];

before(() => {
    coll.drop();
    assert.commandWorked(
        coll.insertMany([
            {_id: 1, x: 1, text: "hello"},
            {_id: 2, x: 2, text: "world"},
            {_id: 3, x: 3, text: "mongo"},
        ]),
    );
});

after(() => {
    coll.drop();
});

describe("globalThis persists across MosJS commands within a query", () => {
    const mapFn = function () {
        if (!globalThis._savedEmit) {
            globalThis._savedEmit = emit;
        }
        emit(this.x, 1);
    };

    const reduceFn = function () {
        // Although this is a very contrived example, it simulates the case where a user stores
        // the emit function from one document's processing and accidentally calls it during the
        // processing of a later document, after the original emitState has been destroyed.
        if (globalThis._savedEmit) {
            globalThis._savedEmit(this.x, 999);
        }
        return 1;
    };

    it("stale emit call from $function throws an error", () => {
        const err = assert.throws(() => {
            coll.aggregate([
                {$sort: {_id: 1}},
                {$limit: 2},
                {
                    $project: {
                        emits: {
                            $_internalJsEmit: {
                                this: "$$ROOT",
                                eval: mapFn,
                            },
                        },
                        x: 1,
                    },
                },
                {
                    $addFields: {
                        _: {
                            $function: {
                                body: reduceFn,
                                args: [],
                                lang: "js",
                            },
                        },
                    },
                },
            ]).toArray();
        });
        assert.eq(err.code, 9712400, err);
    });

    it("cached emit called within $_internalJsEmit acts like a direct emit call", () => {
        // When _savedEmit is invoked from inside a $_internalJsEmit eval, emitData is still
        // live, so it routes to the current EmitState and produces a real KVP — identical to
        // calling emit() directly.
        const results = coll
            .aggregate([
                {$sort: {_id: 1}},
                {$limit: 2},
                {
                    $project: {
                        emits: {
                            $_internalJsEmit: {
                                this: "$$ROOT",
                                eval: mapFn,
                            },
                        },
                        x: 1,
                    },
                },
                {
                    $project: {
                        emits: {
                            $_internalJsEmit: {
                                this: "$$ROOT",
                                eval: function () {
                                    if (!globalThis._cachedEmit) {
                                        globalThis._cachedEmit = emit;
                                    }
                                    emit(this.x, 1);
                                    globalThis._cachedEmit(this.x, 999);
                                },
                            },
                        },
                        x: 1,
                    },
                },
            ])
            .toArray();

        for (let i = 0; i < results.length; i++) {
            const rEmits = results[i].emits;
            assert.eq(rEmits.length, 2, `Expected 2 emits: ${tojson(rEmits)}`);
            assert.eq(rEmits[0], {k: i + 1, v: 1}, results[0]);
            assert.eq(rEmits[1], {k: i + 1, v: 999}, results[0]);
        }
    });

    it("stale emit in mapreduce throws an error", () => {
        assert.commandFailedWithCode(
            db.runCommand({
                mapReduce: coll.getName(),
                map: mapFn,
                reduce: reduceFn,
                out: {inline: 1},
            }),
            9712400,
        );
    });

    it("A stored emit reference should not be available in subsequent queries", () => {
        coll.aggregate([
            {$sort: {_id: 1}},
            {$limit: 2},
            {
                $project: {
                    emits: {
                        $_internalJsEmit: {
                            this: "$$ROOT",
                            eval: mapFn,
                        },
                    },
                    x: 1,
                },
            },
        ]).toArray();

        const results = coll
            .aggregate([
                {$limit: 1},
                {
                    $addFields: {
                        found: {
                            $function: {
                                body: function () {
                                    return typeof globalThis._savedEmit;
                                },
                                args: [],
                                lang: "js",
                            },
                        },
                    },
                },
            ])
            .toArray();

        assert.eq(results[0].found, "undefined", results);
    });
});

describe("BSON wrappers retained in globalThis are invalidated across invocations", () => {
    // A $function evaluation retains its argument wrapper in globalThis; the stack-local
    // BSONObjBuilder backing it is destroyed when the evaluation returns.
    const stashFn = function (arg) {
        globalThis._savedWrapper = arg;
        return 1;
    };

    it("wrappers created by $function are invalid in subsequent $accumulator", () => {
        // SECBUG-5387: The $accumulator init re-enters the same operation-cached scope
        // that ran the $function, so the retained wrapper must be rejected there rather
        // than read as freed memory.
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {$sort: {_id: 1}},
                {$limit: 1},
                {
                    $project: {
                        seed: {
                            $function: {
                                body: stashFn,
                                args: ["$$ROOT"],
                                lang: "js",
                            },
                        },
                    },
                },
                {
                    $group: {
                        _id: null,
                        result: {
                            $accumulator: {
                                initArgs: [1],
                                init: function () {
                                    return globalThis._savedWrapper.x;
                                },
                                accumulate: function (state, seed) {
                                    return state;
                                },
                                accumulateArgs: ["$seed"],
                                merge: function (left, right) {
                                    return left;
                                },
                                lang: "js",
                            },
                        },
                    },
                },
            ],
            cursor: {},
        });
        assert.commandFailedWithCode(res, ErrorCodes.BadValue);
        assert(/Attempt to access an invalidated BSON Object in JS scope/.test(res.errmsg), res);
    });

    it("wrappers created by $function are invalid in function compilation", () => {
        // SECBUG-5396: createFunction() is not a parse; a body beginning with an anonymous
        // function(){} is evaluated verbatim, so the side effects of a comma expression
        // run during compilation, before the barrier that protects the function body. The
        // retained wrapper must be invalid there too.
        const res = db.runCommand({
            aggregate: coll.getName(),
            pipeline: [
                {$sort: {_id: 1}},
                {$limit: 1},
                {
                    $project: {
                        seed: {
                            $function: {
                                body: stashFn,
                                args: ["$$ROOT"],
                                lang: "js",
                            },
                        },
                    },
                },
                {
                    $project: {
                        result: {
                            $function: {
                                body: "function(){}, globalThis._savedWrapper.x, function() { return 0; }",
                                args: [1],
                                lang: "js",
                            },
                        },
                    },
                },
            ],
            cursor: {},
        });
        assert.commandFailedWithCode(res, ErrorCodes.BadValue);
        assert(/Attempt to access an invalidated BSON Object in JS scope/.test(res.errmsg), res);
    });
});
