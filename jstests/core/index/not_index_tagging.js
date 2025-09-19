/**
 * Verifies that index-ineligible predicates under a $not do not get tagged with an index.
 */

import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {describe, it} from "jstests/libs/mochalite.js";

const coll = db.not_index_tagging;
coll.drop();
assert.commandWorked(coll.insert({a: [1, 2, 3]}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({"a.b": 1}));

describe("Index-ineligible predicates under $not do not get tagged with an index", function () {
    const assertUsesCollScan = (pred, collation = {}) => {
        const explain = assert.commandWorked(coll.find(pred).collation(collation).explain());
        assert(isCollscan(db, getWinningPlanFromExplain(explain)), tojson(explain));
    };

    it("should not use a multikey index with $expr", function () {
        assertUsesCollScan({$nor: [{$expr: {$eq: ["$a", 5]}}]});
        assertUsesCollScan({$or: [{b: {$lte: new Date()}}, {$nor: [{$expr: {$gte: [10, "$a"]}}]}]});
    });

    it("should not use an index with $_internalEqHash", function () {
        assertUsesCollScan({a: {$not: {$_internalEqHash: NumberLong(2)}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: {$_internalEqHash: NumberLong(2)}}}]});
    });

    it("should not use an index when the query is specified with a collator different from the index", function () {
        assertUsesCollScan({a: {$not: {$eq: "string"}}}, {locale: "fr"});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: {$eq: "string"}}}]}, {locale: "fr"});
    });

    it("should not use an index with $mod", function () {
        assertUsesCollScan({a: {$not: {$mod: [2, 1]}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: {$mod: [2, 1]}}}]});
    });

    it("should not use an index with $regex", function () {
        assertUsesCollScan({a: {$not: /abc/}});
        assertUsesCollScan({a: {$not: {$in: [1, 2, /abc/, 3]}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: /abc/}}]});
    });

    // $not + elemMatch is not eligible to use an index. We cannot build index bounds for "a" because
    // $not inverts the $elemMatch semantics, which must be applied to the entire array.
    it("should not use an index with $elemMatch", function () {
        // Value $elemMatch.
        assertUsesCollScan({a: {$not: {$elemMatch: {$gt: 1}}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: {$elemMatch: {$gt: 1}}}}]});

        // Object $elemMatch.
        assertUsesCollScan({a: {$not: {$elemMatch: {b: 2}}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$not: {$elemMatch: {b: 2}}}}]});
    });

    it("should not use an index with $elemMatch + $_internalEqHash", function () {
        assertUsesCollScan({a: {$elemMatch: {$not: {$_internalEqHash: NumberLong(2)}}}});
        assertUsesCollScan({$or: [{b: {$eq: 5}}, {a: {$elemMatch: {$not: {$_internalEqHash: NumberLong(2)}}}}]});
    });

    it("should not use index with $not under object $elemMatch", function () {
        assertUsesCollScan({a: {$elemMatch: {b: {$not: {$eq: 2}}}}});
    });
});

describe("Index-eligible predicates under $not do get tagged with an index", function () {
    const assertDoesNotUseCollScan = (pred) => {
        const explain = assert.commandWorked(coll.find(pred).explain());
        assert(!isCollscan(db, getWinningPlanFromExplain(explain)), tojson(explain));
    };

    it("should use an index with $not + $eq-null", function () {
        assertDoesNotUseCollScan({b: {$not: {$eq: null}}});
    });

    // *value* $elemMatch + $not with index-eligible predicates *is* eligible. Unlike the $not +
    // elemMatch case above, we can build bounds for an index on "a" by inverting the $gt:1 bounds.
    it("should use an index with value $elemMatch + $not", function () {
        // Value $elemMatch.
        assertDoesNotUseCollScan({a: {$elemMatch: {$not: {$gt: 1}}}});
    });
});
