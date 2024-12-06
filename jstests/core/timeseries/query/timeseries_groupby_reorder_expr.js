/**
 * Test the behavior of $group on time-series collections. Specifically, we are targeting rewrites
 * that replace bucket unpacking with $group over the buckets collection, where the group key
 * expressions are non-trivial, i.e., more than just a field path or a constant.
 *
 * @tags: [
 *   directly_against_shardsvrs_incompatible,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_72,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

import {
    runGroupRewriteTest
} from 'jstests/core/timeseries/libs/timeseries_groupby_reorder_helpers.js';

// The rewrite applies here because only the metafield is accessed in the group key, and only min or
// max is used in the accumulators.
(function testMetaGroupKeyMinMaxExpr() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 'foo', val: 10},
        {time: t, myMeta: 'Foo', val: 30},
        {time: t, myMeta: 'Bar', val: 50},
        {time: t, myMeta: 'baR', val: 70},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {$toUpper: ['$myMeta']}, min: {$min: '$val'}, max: {$max: '$val'}}}],
        [{_id: 'FOO', min: 10, max: 30}, {_id: 'BAR', min: 50, max: 70}]);
})();

// Similar to the above, but with a more complex group expression.
// - Multiple fields nested in the metaField
// - Multiple fields being grouped on
// - Deeper expression tree for groupby expression
(function testMetaGroupKeyMinMaxComplexExpr() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: {a: 'foo', b: 'bar'}, val: 10},
        {time: t, myMeta: {a: 'fOo', b: 'bAr'}, val: 30},
        {time: t, myMeta: {a: 'foO', b: 'baR'}, val: 50},
        {time: t, myMeta: {a: 'Foo', b: 'Bar'}, val: 70},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{
            $group: {
                _id: {
                    u: {$concat: [{$toUpper: '$myMeta.a'}, '-', {$toUpper: '$myMeta.b'}]},
                    l: {$concat: [{$toLower: '$myMeta.a'}, '-', {$toLower: '$myMeta.b'}]}
                },
                min: {$min: '$val'},
                max: {$max: '$val'}
            }
        }],
        [{_id: {u: 'FOO-BAR', l: 'foo-bar'}, min: 10, max: 70}]);
})();

// The rewrite does not apply here because a non-metafield is accessed in the group key, even
// though only min and max is used in the accumulators.
(function testNonMetaGroupKeyMinMaxExpr() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 1, key: 'a', val: 10},
        {time: t, myMeta: 2, key: 'A', val: 30},
        {time: t, myMeta: 2, key: 'B', val: 50},
        {time: t, myMeta: 1, key: 'b', val: 70},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {$toUpper: ['$key']}, min: {$min: '$val'}, max: {$max: '$val'}}}],
        [{_id: 'A', min: 10, max: 30}, {_id: 'B', min: 50, max: 70}]);
})();

// The rewrite does not apply here, but the rewrite code needs to take care to leave the grouping
// expressions unchanged. The rewrite to "m" occurs, but then "k" references a non-metafield, so the
// rewrite is discareded.
(function testNonMetaGroupKeyMinMaxExprMultiKey() {
    const t = new Date();
    const docs = [
        {time: t, myMeta: 'foo', key: 'a', val: 10},
        {time: t, myMeta: 'Foo', key: 'A', val: 30},
        {time: t, myMeta: 'Bar', key: 'B', val: 50},
        {time: t, myMeta: 'baR', key: 'b', val: 70},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{
            $group: {
                _id: {
                    m: {$toUpper: ['$myMeta']},  // this gets rewritten and discarded
                    k: {$toUpper: ['$key']}      // <-- this prevents group reordering
                },
                min: {$min: '$val'},
                max: {$max: '$val'}
            }
        }],
        [{_id: {m: 'FOO', k: 'A'}, min: 10, max: 30}, {_id: {m: 'BAR', k: 'B'}, min: 50, max: 70}]);
})();

// The rewrite does not apply here because there is no metaField for the time series collection. Any
// field path in the $group _id means the optimization cannot apply.
(function testNonMetaGroupKeyMinMaxExprNoMetaField() {
    const t = new Date();
    const docs = [
        {time: t, myField: 'foo', val: 10},
        {time: t, myField: 'Foo', val: 30},
        {time: t, myField: 'Bar', val: 50},
        {time: t, myField: 'baR', val: 70},
    ];
    runGroupRewriteTest(
        coll,
        docs,
        [{$group: {_id: {m: {$toUpper: ['$myField']}}, min: {$min: '$val'}, max: {$max: '$val'}}}],
        [{_id: {m: 'FOO'}, min: 10, max: 30}, {_id: {m: 'BAR'}, min: 50, max: 70}],
        true  // excludeMeta
    );
})();
