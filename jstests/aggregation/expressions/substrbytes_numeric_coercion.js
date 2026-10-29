/**
 * SERVER-136265: $substrBytes numeric coercion agrees between classic and SBE.
 * @tags: [
 *   # Requires binaries with the SERVER-136265 SBE coercion fix.
 *   requires_fcv_91,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$substrBytes numeric coercion", function () {
    const coll = db[jsTestName()];

    before(function () {
        coll.drop();
        assert.commandWorked(coll.insert({y: "abc", z: "abcdefghij"}));
    });

    after(function () {
        assert(coll.drop());
    });

    function assertSubstring(expected, offset, length, str = "$y") {
        // The field path prevents constant folding so the execution engine performs the coercion.
        assert.eq(
            [{_id: null, a: [expected]}],
            coll
                .aggregate({
                    $group: {_id: null, a: {$push: {$substrBytes: [str, offset, length]}}},
                })
                .toArray(),
        );
    }

    function assertException(offset, length) {
        assert.commandFailed(
            coll.runCommand("aggregate", {
                pipeline: [
                    {$group: {_id: null, a: {$push: {$substrBytes: ["$y", offset, length]}}}},
                ],
                cursor: {},
            }),
        );
    }

    it("truncates doubles", function () {
        assertSubstring("bc", 1.5, 2);
        assertSubstring("b", 1, 1.5);
        assertSubstring("ab", -0.5, 2);
        assertSubstring("", 1, -0.5);
        assertSubstring("bc", 1, -1.5);
    });

    it("rounds decimals to nearest with ties to even", function () {
        assertSubstring("c", NumberDecimal("1.5"), 2);
        assertSubstring("c", NumberDecimal("2.5"), 2);
        assertSubstring("ab", 0, NumberDecimal("1.5"));
        assertSubstring("ab", NumberDecimal("-0.5"), 2);
    });

    it("preserves int64 precision", function () {
        assertSubstring("", NumberLong("9223372036854775807"), 2);
        assertSubstring("bc", 1, NumberLong("-9223372036854775808"));
        assertSubstring("", NumberDecimal("9223372036854775807.4"), 2);
        assertSubstring("bc", 1, NumberDecimal("-9223372036854775808.4"));
    });

    it("coerces numeric arguments in nested expressions", function () {
        assertSubstring("e", 1.5, 1.5, {
            $substrBytes: [{$substrBytes: ["$z", 1.5, 6.5]}, 2.5, 5.5],
        });
    });

    it("rejects non-finite values, overflow, and negative offsets after coercion", function () {
        assertException(NaN, 2);
        assertException(1, NaN);
        assertException(Infinity, 2);
        assertException(1, Infinity);
        assertException(-Infinity, 2);
        assertException(1, -Infinity);
        assertException(9223372036854775808, 2);
        assertException(1, 9223372036854775808);
        assertException(NumberDecimal("NaN"), 2);
        assertException(1, NumberDecimal("NaN"));
        assertException(NumberDecimal("Infinity"), 2);
        assertException(1, NumberDecimal("Infinity"));
        assertException(NumberDecimal("9223372036854775807.5"), 2);
        assertException(1, NumberDecimal("9223372036854775807.5"));
        assertException(NumberDecimal("-9223372036854775808.6"), 2);
        assertException(1, NumberDecimal("-9223372036854775808.6"));
        assertException(-1.5, 2);
        assertException(NumberDecimal("-0.6"), 2);
    });
});
