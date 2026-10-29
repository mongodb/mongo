/**
 * Regression test for SERVER-131802. The boolean expression simplifier incorrectly rewrote a
 * tautological $or that contains a predicate and its negation as separate single-literal disjuncts,
 * dropping documents from the result set.
 *
 * The filter (a & b!=1) | (a!=1) | (a & b) is always true, so it must match every document, but the
 * bad rewrite collapsed it to just {a: 1} and dropped every document with a != 1.
 */
import {describe, it} from "jstests/libs/mochalite.js";

describe("boolean expression simplifier", function () {
    it("does not drop documents for a tautological $or", function () {
        const coll = db[jsTestName()];
        coll.drop();
        const docs = [
            {_id: 0, a: 1, b: 1},
            {_id: 1, a: 1, b: 2},
            {_id: 2, a: 2, b: 1},
            {_id: 3, a: 2, b: 2},
        ];
        assert.commandWorked(coll.insert(docs));

        const filter = {$or: [{a: 1, b: {$ne: 1}}, {a: {$ne: 1}}, {a: 1, b: 1}]};
        const results = coll.find(filter).toArray();

        assert.eq(docs.length, results.length, "tautological $or must match every document", {
            results,
        });
    });

    it("does not drop documents for a tautological $or with a range predicate", function () {
        const coll = db[jsTestName()];
        coll.drop();
        const docs = [
            {_id: 0, a: 10, b: 1},
            {_id: 1, a: 10, b: 2},
            {_id: 2, a: 3, b: 1},
            {_id: 3, a: 3, b: 2},
        ];
        assert.commandWorked(coll.insert(docs));

        // (a>5 & b!=1) | !(a>5) | (a>5 & b=1) is a tautology: every document satisfies at least one
        // of the three branches, so the filter must match everything.
        const filter = {
            $or: [{a: {$gt: 5}, b: {$ne: 1}}, {a: {$not: {$gt: 5}}}, {a: {$gt: 5}, b: 1}],
        };
        const results = coll.find(filter).toArray();

        assert.eq(docs.length, results.length, "tautological $or must match every document", {
            results,
        });
    });
});
