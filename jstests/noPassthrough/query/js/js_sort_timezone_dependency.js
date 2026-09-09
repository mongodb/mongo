/**
 * Regression test for JavaScript timezone dependency
 *
 * The legacy MozJS engine used the host timezone when stringifying dates, while the WASM-based
 * engine always uses UTC. This means that server-side JavaScript sort functions could produce
 * different results between different server versions and on different hosts.
 *
 * This test pins down that hazard by running a pipeline involving a js sort against servers
 * launched under different timezones and requiring the results to match. The fuzzer should no
 * longer generate such pipelines, but this test ensures that similar timezone-dependent behaviour
 * isn't reintroduced in the future.
 *
 * @tags: [
 *   requires_scripting,
 * ]
 */

import {before, describe, it} from "jstests/libs/mochalite.js";

describe("server-side JavaScript Array.sort() in $accumulator", function () {
    const pipeline = [
        {
            $group: {
                _id: null,
                array: {
                    $accumulator: {
                        init: function () {
                            return [];
                        },
                        accumulate: function (state, input) {
                            state.push(input);
                            return state.sort();
                        },
                        accumulateArgs: ["$obj.array"],
                        merge: function (state1, state2) {
                            return state1.concat(state2).sort();
                        },
                        lang: "js",
                    },
                },
            },
        },
    ];
    let baseline_arr;

    /**
     * Starts a mongod with TZ set to 'timezone', constructs a small collection, runs 'pipeline',
     * and returns the accumulated array.
     * The collection contains two documents each containing a date. These dates may have different
     * orderings when stringified in different timezones.
     * For example in UTC the first date is a Thursday while the second date is a Saturday,
     * so the order would be ["Sat Aug 24 ...", "Thu Mar 21 ..."]. In Europe/Paris the first date
     * is a Friday so the ordering would be ["Fri Mar 22 ...", "Sat Aug 24 ..."].
     */
    function runUnderTimezone(timezone) {
        const conn = MongoRunner.runMongod({env: {TZ: timezone}});
        assert.neq(conn, null, "mongod failed to start", {timezone});
        try {
            const coll = conn.getDB(jsTestName())["accumulator_js_sort"];
            const kDocs = [
                {_id: 0, obj: {array: [ISODate("2019-03-21T23:07:53.146Z")]}},
                {_id: 1, obj: {array: [ISODate("2019-08-24T06:30:28.864Z")]}},
            ];
            assert.commandWorked(coll.insert(kDocs));

            const results = coll.aggregate(pipeline).toArray();
            assert.eq(1, results.length, "expected exactly one group result", {results});
            jsTest.log.info("Accumulated array", {timezone, array: results[0].array});
            return results[0].array;
        } finally {
            MongoRunner.stopMongod(conn);
        }
    }

    before(function () {
        baseline_arr = runUnderTimezone("UTC");
    });

    for (const timezone of ["Europe/Paris", "Asia/Tokyo"]) {
        it(`sorts independently of the host timezone under ${timezone}`, function () {
            assert.eq(
                runUnderTimezone(timezone),
                baseline_arr,
                "accumulated array does not match the baseline ordering",
                {timezone},
            );
        });
    }
});
