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
