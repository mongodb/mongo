/**
 * Tests that the discrete median accumulator semantics matches the percentile semantics with the
 * field 'p':[0.5].
 * @tags: [
 *   requires_fcv_81,
 *   # TODO SERVER-91581: Support spilling
 *   incompatible_aubsan,
 *   # TODO SERVER-91582: Support sharding
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {
    testWithMultipleGroupsMedian,
    testWithSingleGroupMedian
} from "jstests/aggregation/libs/percentiles_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const coll = db[jsTestName()];

if (FeatureFlagUtil.isPresentAndEnabled(db, "AccuratePercentiles")) {
    /**
     * Tests for correctness without grouping. Confirms that $median computes the expected value and
     * also checks that $percentile for p=0.5 computes the same value, because $median is supposed
     * to be completely equivalent to the latter (e.g. we should not optimize $median independently
     * of $percentile).
     */
    testWithSingleGroupMedian({
        coll: coll,
        docs: [{x: 0}, {x: "non-numeric"}, {x: 1}, {no_x: 0}, {x: 2}],
        medianSpec: {$median: {input: "$x", method: "discrete"}},
        expectedResult: 1,
        msg: "Non-numeric data should be ignored"
    });

    testWithSingleGroupMedian({
        coll: coll,
        docs: [{x: "non-numeric"}, {non_x: 1}],
        medianSpec: {$median: {input: "$x", method: "discrete"}},
        expectedResult: null,
        msg: "Median of completely non-numeric data."
    });

    testWithMultipleGroupsMedian({
        coll: coll,
        docs: [{k: 0, x: 2}, {k: 0, x: 1}, {k: 1, x: 2}, {k: 2}, {k: 0, x: "str"}, {k: 1, x: 0}],
        medianSpec: {$median: {input: "$x", method: "discrete"}},
        expectedResult: [/* k:0 */ 1, /* k:1 */ 0, /* k:2 */ null],
        msg: "Median of multiple groups"
    });
}
