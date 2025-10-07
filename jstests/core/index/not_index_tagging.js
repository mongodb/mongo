/**
 * Verifies that index-ineligible predicates under a $not do not get tagged with an index.
 */

import {getWinningPlanFromExplain, isCollscan} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";
import {describe, it} from "jstests/libs/mochalite.js";

const coll = db.not_index_tagging;
coll.drop();
assert.commandWorked(coll.insert({a: [1, 2, 3]}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({b: 1}));
assert.commandWorked(coll.createIndex({"a.b": 1}));

const isSbeEnabled = checkSbeFullyEnabled(db);

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

    // These hit uassert 40353 with SBE fully enabled.
    if (!isSbeEnabled) {
        it("should not use an index with $elemMatch + $not + $_internalExprEq-null", function () {
            assertUsesCollScan({b: {$elemMatch: {$not: {$_internalExprEq: null}}}});
        });

        it("should not use an index with $elemMatch + $not + $_internalExprLt-null", function () {
            assertUsesCollScan({b: {$elemMatch: {$not: {$_internalExprLt: null}}}});
        });

        it("should not use an index with $elemMatch + $not + $_internalExprGte-null", function () {
            assertUsesCollScan({b: {$elemMatch: {$not: {$_internalExprGte: null}}}});
        });

        it("should not use index with $not + $elemMatch + $_internalExprLt-null", function () {
            assertUsesCollScan({b: {$not: {$elemMatch: {$_internalExprLt: null}}}});
        });
    }

    it("should not use index with $nor + $internalExprLt-null and another predicate", function () {
        assertUsesCollScan({$nor: [{a: {$_internalExprEq: 1}}, {b: {$_internalExprLt: null}}]});
    });

    it("should not use index with $nor + $internalExprEq-null", function () {
        assertUsesCollScan({$nor: [{b: {$_internalExprEq: null}}]});
    });

    it("should not use index with $nor + $internalExprLt-null", function () {
        assertUsesCollScan({$nor: [{b: {$_internalExprLt: null}}]});
    });

    it("should not use index with $nor + $internalExprGte-null", function () {
        assertUsesCollScan({$nor: [{b: {$_internalExprGte: null}}]});
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

    it("should use index with $nor + $internalExprEq-integer", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprEq: 123}}]});
    });

    it("should use index with $nor + $internalExprLt-integer", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprLt: 123}}]});
    });

    it("should use index with $nor + $internalExprLte-integer", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprLte: 123}}]});
    });

    it("should use index with $nor + $internalExprGt-integer", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprGt: 123}}]});
    });

    it("should use index with $nor + $internalExprGte-integer", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprGte: 123}}]});
    });

    it("should use index with $nor + $internalExprLte-null", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprLte: null}}]});
    });

    it("should use index with $nor + $internalExprGt-null", function () {
        assertDoesNotUseCollScan({$nor: [{b: {$_internalExprGt: null}}]});
    });

    // These hit uassert 40353 with SBE fully enabled.
    if (!isSbeEnabled) {
        it("should use an index with $elemMatch + $not + $_internalExprGt-null", function () {
            assertDoesNotUseCollScan({b: {$elemMatch: {$not: {$_internalExprGt: null}}}});
        });

        it("should use an index with $elemMatch + $not + $_internalExprLte-null", function () {
            assertDoesNotUseCollScan({b: {$elemMatch: {$not: {$_internalExprLte: null}}}});
        });
    }
});
