/**
 * Tests the semantics of the aggregation comparison expressions ($eq, $ne, $gt, $gte, $lt, $lte and
 * $cmp) under $expr, focusing on the parts of the MQL type order that are easy to get wrong:
 * missing fields (Nothing), BSON undefined and the MinKey/MaxKey sentinels.
 *
 * These tests pin down the full ordering for every comparison operator and both operand orders.
 */
import {before, describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];

// The values below span the type order that the comparison expressions use, which is the
// canonicalizeBSONType() order. A missing field ("Nothing") sorts above MinKey but below every real
// value, including null. BSON undefined shares its rank with a missing field, because
// canonicalizeBSONType() maps both EOO and 'undefined' to the same canonical type. Arrays sort above
// strings, so an empty array is just an ordinary array value here:
//
//   MinKey < missing == undefined < null < number < string < [] < MaxKey
//
// Note this is comparison order, not sort-key order: sorting (and index key generation) substitutes
// null for a missing field and undefined for an empty array, which moves both relative to null. The
// comparison expressions make neither substitution, so the two orders genuinely differ.
//
// We encode that order as an integer 'rank' per document and use plain integer comparison in JS as
// an independent oracle for the expected result of each comparison operator. Every value has a
// distinct rank except missing and undefined, which are genuinely equal under MQL comparison, so
// rank equality is exactly MQL equality.
const docs = [
    {_id: "minkey", f: MinKey},
    {_id: "missing"}, // 'f' is absent -> Nothing
    {_id: "undefined", f: undefined}, // BSON undefined, compares equal to missing
    {_id: "null", f: null},
    {_id: "number", f: 5},
    {_id: "string", f: "hello"},
    {_id: "emptyArray", f: []},
    {_id: "maxkey", f: MaxKey},
];
const rank = {
    minkey: 0,
    missing: 1,
    undefined: 1,
    null: 2,
    number: 3,
    string: 4,
    emptyArray: 5,
    maxkey: 6,
};

// Right-hand-side literals. These are always present values (a literal can never be "missing"), so
// their ranks come from the same order above.
const rhsValues = [
    {name: "MinKey", value: MinKey, rank: 0},
    {name: "undefined", value: undefined, rank: 1},
    {name: "null", value: null, rank: 2},
    {name: "number", value: 5, rank: 3},
    {name: "string", value: "hello", rank: 4},
    {name: "emptyArray", value: [], rank: 5},
    {name: "MaxKey", value: MaxKey, rank: 6},
];

// Boolean comparison operators, each with the reference implementation applied to the operand ranks.
const boolOps = {
    $eq: (a, b) => a === b,
    $ne: (a, b) => a !== b,
    $gt: (a, b) => a > b,
    $gte: (a, b) => a >= b,
    $lt: (a, b) => a < b,
    $lte: (a, b) => a <= b,
};

// Both operand orders must be handled symmetrically.
const orders = [
    {name: "fieldFirst", makeExpr: (op, rhs) => ({[op]: ["$f", rhs]})},
    {name: "fieldSecond", makeExpr: (op, rhs) => ({[op]: [rhs, "$f"]})},
];

describe("$expr comparison semantics for missing/undefined fields and MinKey/MaxKey", function () {
    before(function () {
        coll.drop();
        assert.commandWorked(coll.insertMany(docs));
    });

    // $project form: assert the exact computed result for every document.
    for (const [opName, refImpl] of Object.entries(boolOps)) {
        it(`computes ${opName} across the MQL comparison order, both operand orders`, function () {
            for (const order of orders) {
                for (const rhs of rhsValues) {
                    const expr = order.makeExpr(opName, rhs.value);
                    const results = coll.aggregate([{$project: {v: expr}}]).toArray();
                    for (const doc of results) {
                        const dr = rank[doc._id];
                        const [a, b] =
                            order.name === "fieldFirst" ? [dr, rhs.rank] : [rhs.rank, dr];
                        const expected = refImpl(a, b);
                        assert.eq(doc.v, expected, {
                            msg: "wrong comparison result",
                            operator: opName,
                            order: order.name,
                            rhs: rhs.name,
                            doc: doc._id,
                        });
                    }
                }
            }
        });
    }

    // $cmp returns the three-way result; verify it separately.
    it("computes $cmp across the MQL comparison order, both operand orders", function () {
        for (const order of orders) {
            for (const rhs of rhsValues) {
                const expr = order.makeExpr("$cmp", rhs.value);
                const results = coll.aggregate([{$project: {v: expr}}]).toArray();
                for (const doc of results) {
                    const dr = rank[doc._id];
                    const [a, b] = order.name === "fieldFirst" ? [dr, rhs.rank] : [rhs.rank, dr];
                    const expected = a === b ? 0 : a < b ? -1 : 1;
                    assert.eq(doc.v, expected, {
                        msg: "wrong $cmp result",
                        order: order.name,
                        rhs: rhs.name,
                        doc: doc._id,
                    });
                }
            }
        }
    });

    // $match {$expr} form: assert the set of matched _ids.
    for (const [opName, refImpl] of Object.entries(boolOps)) {
        it(`matches ${opName} across the MQL comparison order, both operand orders`, function () {
            for (const order of orders) {
                for (const rhs of rhsValues) {
                    const expr = order.makeExpr(opName, rhs.value);
                    const matchedIds = coll
                        .aggregate([{$match: {$expr: expr}}, {$project: {_id: 1}}])
                        .toArray()
                        .map((d) => d._id)
                        .sort();
                    const expectedIds = docs
                        .map((d) => d._id)
                        .filter((id) => {
                            const dr = rank[id];
                            const [a, b] =
                                order.name === "fieldFirst" ? [dr, rhs.rank] : [rhs.rank, dr];
                            return refImpl(a, b);
                        })
                        .sort();
                    assert.eq(matchedIds, expectedIds, {
                        msg: "wrong matched set",
                        operator: opName,
                        order: order.name,
                        rhs: rhs.name,
                    });
                }
            }
        });
    }

    // {$gte: MinKey} (and {$gt: MinKey}) must match a missing field, since a missing field is
    // greater than MinKey.
    it("treats a missing field as greater than MinKey (SERVER-131544)", function () {
        assert.eq(
            coll.aggregate([{$match: {$expr: {$gte: ["$f", MinKey]}}}]).itcount(),
            docs.length,
            "{$gte: MinKey} must match every document, including those missing the field",
        );
        assert.eq(
            coll
                .aggregate([{$match: {$expr: {$gt: ["$f", MinKey]}}}, {$project: {_id: 1}}])
                .toArray()
                .map((d) => d._id)
                .sort(),
            docs
                .filter((d) => d._id !== "minkey")
                .map((d) => d._id)
                .sort(),
            "{$gt: MinKey} must match every document except the one whose field is MinKey",
        );
    });
});
