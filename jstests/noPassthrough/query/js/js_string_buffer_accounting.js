/**
 * Runs a $function that repeatedly builds and discards long strings, whose characters live in
 * mozilla::StringBuffer objects allocated from StringBufferArena, under a small jsHeapLimitMB.
 *
 * @tags: [
 *   requires_scripting,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {isMozjsWasm} from "jstests/libs/js_engine_util.js";

describe("legacy MozJS StringBuffer accounting", function () {
    const JS_HEAP_LIMIT_MB = 20;
    // Must match the literals inside joinManyStrings(), which is serialized and run server-side.
    const EXPECTED = 1000 * 10 * 5000;

    let conn;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {jsHeapLimitMB: JS_HEAP_LIMIT_MB}});
        const db = conn.getDB(jsTestName());
        if (isMozjsWasm(db)) {
            jsTest.log.info("Skipping: test exercises the legacy MozJS allocator, not mozjs-wasm");
            MongoRunner.stopMongod(conn);
            quit();
        }
        coll = db.coll;
        assert.commandWorked(coll.insertOne({_id: 0}));
    });

    after(function () {
        if (conn) {
            MongoRunner.stopMongod(conn);
        }
    });

    // Builds a 1000-element array of 10-char strings and joins it 5000 times, returning the sum
    // of the joined lengths.
    function joinManyStrings() {
        const a = [];
        for (let i = 0; i < 1000; i++) {
            a.push("xxxxxxxxxx");
        }
        let t = 0;
        for (let i = 0; i < 5000; i++) {
            t += a.join("").length;
        }
        return t;
    }

    it("aggregates repeated long-string allocations within jsHeapLimitMB", function () {
        // Run the aggregation a few times; each call uses a fresh scope.
        for (let i = 0; i < 3; i++) {
            const res = coll
                .aggregate([
                    {$limit: 1},
                    {
                        $project: {
                            r: {
                                $function: {
                                    body: joinManyStrings,
                                    args: [],
                                    lang: "js",
                                },
                            },
                        },
                    },
                ])
                .toArray();
            assert.eq(res.length, 1, "unexpected empty aggregation result", {res, iteration: i});
            assert.eq(res[0].r, EXPECTED, "unexpected $function result", {res, iteration: i});
        }
    });
});
