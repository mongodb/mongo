/**
 * Tests the scoping behavior of globalThis state (e.g. variables stored on the global object)
 * across MozJS invocations in aggregation expressions.
 *
 * @tags: [
 *   requires_scripting,
 *   requires_fcv_90,
 * ]
 */
import {isMozjsWasm} from "jstests/libs/js_engine_util.js";
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

describe("globalThis persists across MozJS commands within a query", () => {
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

// TODO SERVER-135002: JS_SetProperty is [[Set]], so it honours a setter the script installed
// on this name, turning a host-initiated write into an execution point for user JS.
// It needs to be replaced with JS_DefineProperty ([[DefineOwnProperty]]), which should prevent
// this scenario from occuring.
describe("BSON wrappers do not outlive the invocation that created them", () => {
    const initFn = function (doc) {
        globalThis.__saved = doc;
        Object.defineProperty(globalThis, "__accumulate", {
            configurable: true,
            set: function (fn) {
                try {
                    globalThis.__leaked = String(globalThis.__saved.x);
                } catch (e) {
                    globalThis.__staleReadErr = String(e);
                }
                Object.defineProperty(globalThis, "__accumulate", {
                    value: fn,
                    writable: true,
                    configurable: true,
                });
            },
        });
        return {observed: false, read: false, err: ""};
    };

    // Runs after the setter has fired, and reports what the stale resolve did. `observed` marks
    // this as a state from a node that actually ran the setter, which mergeFn relies on below.
    const accumulateFn = function (state, val) {
        return {
            observed: true,
            read: globalThis.__leaked !== undefined,
            err: globalThis.__staleReadErr || "",
        };
    };

    // On a sharded collection the merging node runs init() and is then assigned __merge, never
    // __accumulate -- so its own state never observes the setter. Returning it would report
    // {read: false, err: ""} and mask the result from the shard that did observe. Prefer a state
    // that saw the stale read succeed (the failure under test), then any observation at all.
    const mergeFn = function (a, b) {
        if (a.read) {
            return a;
        }
        if (b.read) {
            return b;
        }
        return a.observed ? a : b;
    };

    it("a wrapper retained by $accumulator init is rejected when a global setter resolves it", () => {
        if (!isMozjsWasm(db)) {
            jsTest.log.info(
                "Skipping: the generation counter under test is specific to mozjs-wasm",
            );
            return;
        }

        // A mixed-version cluster can execute this $accumulator on a shard whose binary predates
        // the guard (SERVER-134893). Those binaries report mozjs-wasm, so the check above does
        // not catch them, and they fail here for the right reason: the retained wrapper really is
        // dereferenced, and surfaces as a malformed-BSON error from the freed chunk rather than
        // the guard's own error. Asserting another binary's behaviour is not this test's job.
        // Remove once a release carrying the fix is the last-LTS binary.
        if (TestData.multiversionBinVersion || TestData.mixedBinVersions) {
            jsTest.log.info(
                "Skipping: mixed-version cluster may run this on a binary predating the fix",
            );
            return;
        }

        const res = coll
            .aggregate([
                {$limit: 1},
                {
                    $group: {
                        _id: null,
                        out: {
                            $accumulator: {
                                init: initFn,
                                initArgs: [{x: 42}],
                                accumulate: accumulateFn,
                                accumulateArgs: ["$x"],
                                merge: mergeFn,
                                lang: "js",
                            },
                        },
                    },
                },
            ])
            .toArray();

        assert.eq(res.length, 1, res);
        const out = res[0].out;

        assert.eq(
            out.read,
            false,
            "the setter resolved a wrapper over the freed init() argument buffer instead of being rejected",
            {out},
        );
        assert(
            out.err.includes("Attempt to access an invalidated BSON Object"),
            "expected the retained wrapper to be rejected by the generation guard",
            {out},
        );
    });
});
