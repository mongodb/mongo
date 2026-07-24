/**
 * SERVER-109615: $zip's 'defaults' argument accepts any expression that resolves to an array at
 * runtime (e.g. a field path), in addition to an array literal of per-element expressions.
 *
 * @tags: [
 *   # The whole-array defaults mode is only supported by binaries with SERVER-109615.
 *   requires_fcv_90,
 * ]
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$zip with 'defaults' given as a whole-array expression", function () {
    let coll;

    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.insert({"long": [1, 2, 3], "short": ["x", "y"], "dollars": ["$a", "$b"]}),
        );
    });

    function zipResult(zipObj) {
        const output = coll.aggregate([{$project: {zipped: {$zip: zipObj}}}]).toArray();
        assert.eq(1, output.length, "expected exactly one result document", {output});
        return output[0].zipped;
    }

    it("accepts a field path resolving to an array", function () {
        const zipped = zipResult({
            inputs: [
                [1, 2, 3],
                ["A", "B"],
            ],
            defaults: "$short",
            useLongestLength: true,
        });
        assert.eq(zipped, [
            [1, "A"],
            [2, "B"],
            [3, "y"],
        ]);
    });

    it("accepts an operator expression resolving to an array", function () {
        const zipped = zipResult({
            inputs: [
                [1, 2, 3],
                ["A", "B"],
            ],
            defaults: {$concatArrays: [["C"], ["D"]]},
            useLongestLength: true,
        });
        assert.eq(zipped, [
            [1, "A"],
            [2, "B"],
            [3, "D"],
        ]);
    });

    it("treats elements of a data-derived defaults array as inert values", function () {
        // A string like "$b" coming from a document is not re-interpreted as a field path.
        const zipped = zipResult({
            inputs: [
                [1, 2, 3],
                ["A", "B"],
            ],
            defaults: "$dollars",
            useLongestLength: true,
        });
        assert.eq(zipped, [
            [1, "A"],
            [2, "B"],
            [3, "$b"],
        ]);
    });

    it("evaluates elements of a literal defaults array as expressions", function () {
        // Unlike a data-derived array, the elements of a literal defaults array are expressions:
        // field paths and operators are evaluated per element.
        const zipped = zipResult({
            inputs: [
                [1, 2, 3],
                ["A", "B"],
            ],
            defaults: [{$toUpper: "pad"}, "$short"],
            useLongestLength: true,
        });
        assert.eq(zipped, [
            [1, "A"],
            [2, "B"],
            [3, ["x", "y"]],
        ]);
    });

    it("treats a nullish defaults expression the same as omitting 'defaults'", function () {
        const zipped = zipResult({
            inputs: [
                [1, 2, 3],
                ["A", "B"],
            ],
            defaults: "$missingField",
            useLongestLength: true,
        });
        assert.eq(zipped, [
            [1, "A"],
            [2, "B"],
            [3, null],
        ]);
    });

    it("fails when the defaults expression does not resolve to an array", function () {
        assertErrorCode(
            coll,
            [
                {
                    $project: {
                        zipped: {
                            $zip: {
                                inputs: [[1, 2], [1]],
                                defaults: {"a": "b"},
                                useLongestLength: true,
                            },
                        },
                    },
                },
            ],
            10961500,
            "defaults did not resolve to an array",
        );
    });

    it("fails when the resolved defaults array length does not match inputs", function () {
        assertErrorCode(
            coll,
            [
                {
                    $project: {
                        zipped: {
                            $zip: {
                                inputs: [[1, 2, 3], ["A", "B"], [true]],
                                defaults: "$short",
                                useLongestLength: true,
                            },
                        },
                    },
                },
            ],
            10961501,
            "defaults resolved to an array whose length does not match inputs",
        );
    });

    it("still requires useLongestLength with expression defaults", function () {
        assertErrorCode(
            coll,
            [{$project: {zipped: {$zip: {inputs: [["A"]], defaults: "$short"}}}}],
            34466,
            "cannot specify defaults unless useLongestLength is true",
        );
    });
});
