/**
 * Tests CWI scans when the query contains predicates that cannot be served by tight index bounds either because part
 * of the predicate is on an unindexed path or because the predicate type (null equality) prevents a tight bound on
 * the wildcard field.
 *
 * A CWI {a:1,"b.$**":1,c:1} expands into:
 *  - non-generic entry - concrete subpath (e.g. "b.x"), sparse: no keys for absent fields.
 *  - generic entry - all-values prefix scan ("$_path"): covers any wildcard subpath.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   # Implicit index creation may change the plan/engine used.
 *   assumes_no_implicit_index_creation,
 *   assumes_read_concern_local,
 *   # Uses runWithParamsAllNonConfigNodes which requires a stable shard list.
 *   assumes_stable_shard_list,
 *   # Some expected index bounds require the multi-planner to choose the optimal plan that uses a
 *   # more efficient CWI (non-generic). Sharded suites could mislead the multi-planner to choose a
 *   # worse CWI because the planner may not run sufficient trials if there's no enough docs in some
 *   # shard.
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 * ]
 */
import {assertQueryResults, getAllPlans, getPlanStages} from "jstests/libs/query/analyze_plan.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

// Asserts that each plan contains exactly one IXSCAN matching the descriptor.
function assertAllPlansIxScan(plans, expectedScan) {
    for (const scans of plans) {
        assert.eq(scans.length, 1, "expected exactly one IXSCAN in plan", {scans, expectedScan});
        assert.eq(scans[0].indexName, expectedScan.indexName, "indexName mismatch", {scans});
        assert.eq(scans[0].keyPattern, expectedScan.keyPattern, "keyPattern mismatch", {scans});
        assert.eq(scans[0].indexBounds, expectedScan.indexBounds, "indexBounds mismatch", {scans});
    }
}

function getTestCollection(name) {
    return db[jsTestName() + "_" + name];
}

// Asserts correctness and returns IXSCAN stages for all candidate plans.
function assertResultsAndGetIxscans(coll, query, expected) {
    assertQueryResults(coll, query, expected, {fieldsToSkip: ["_id"]});
    const explain = assert.commandWorked(coll.find(query).explain("allPlansExecution"));
    return getAllPlans(explain).map((plan) => getPlanStages(plan, "IXSCAN"));
}

const kIndexName = "a_1_b.$**_1_c_1";

// $_path bounds for the generic entry
const kGenericPathBounds = ["[MinKey, MinKey]", '["", {})'];

const kGenericScan_a1 = {
    indexName: kIndexName,
    keyPattern: {a: 1, "$_path": 1, c: 1},
    indexBounds: {a: ["[1.0, 1.0]"], "$_path": kGenericPathBounds, c: ["[MinKey, MaxKey]"]},
};
const kGenericScan_a3 = {
    indexName: kIndexName,
    keyPattern: {a: 1, "$_path": 1, c: 1},
    indexBounds: {a: ["[3.0, 3.0]"], "$_path": kGenericPathBounds, c: ["[MinKey, MaxKey]"]},
};

// Predicates that can match missing paths must use the generic expansion, not the sparse
// non-generic expansion (which stores no key for missing fields).
describe("predicates matching missing/null", function () {
    const coll = getTestCollection("missing_path");
    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, b: {x: 1}, c: 1},
                {a: 1, b: {x: -5}, c: 1},
                {a: 1, b: {x: null}, c: 1},
                {a: 1, b: null, c: 1},
                {a: 1, c: 1},
                {a: 2, c: 1},
            ]),
        );
    });

    it("$eq null", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": null, c: 1}, [
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$gte null", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$gte: null}, c: 1}, [
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$lte null", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$lte: null}, c: 1}, [
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$in with null", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$in: [null, 1]}, c: 1}, [
            {a: 1, b: {x: 1}, c: 1},
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$not:{$gt}", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$not: {$gt: 0}}, c: 1}, [
            {a: 1, b: {x: -5}, c: 1},
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$not:{$in}", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$not: {$in: [1, 5]}}, c: 1}, [
            {a: 1, b: {x: -5}, c: 1},
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$exists:false", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$exists: false}, c: 1}, [
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$nin", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$nin: [1, 5]}, c: 1}, [
            {a: 1, b: {x: -5}, c: 1},
            {a: 1, b: {x: null}, c: 1},
            {a: 1, b: null, c: 1},
            {a: 1, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$ne:null excludes null and absent", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": {$ne: null}, c: 1}, [
            {a: 1, b: {x: 1}, c: 1},
            {a: 1, b: {x: -5}, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
});

describe("object-bracket predicates on wildcard", function () {
    const coll = getTestCollection("object_bracket");

    // Non-generic bounds when the wildcard field has object values: $_path covers the exact path
    // AND the sub-path range "b.c." to "b.c/" so that object-valued b.c (indexed at sub-paths)
    // are also found. b.c is fully open since $exists/$type don't restrict the value.
    const kNonGenericScan_bc_object = {
        indexName: kIndexName,
        keyPattern: {a: 1, "$_path": 1, "b.c": 1, c: 1},
        indexBounds: {
            a: ["[3.0, 3.0]"],
            "$_path": ['["b.c", "b.c"]', '["b.c.", "b.c/")'],
            "b.c": ["[MinKey, MaxKey]"],
            c: ["[-inf, 3.0)"],
        },
    };

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 3, b: {c: 1}, c: 2},
                {a: 3, b: {c: {nested: 1}}, c: 2},
                {a: 3, b: {c: {}}, c: 2},
                {a: 3, b: {c: null}, c: 2},
                {a: 3, c: 2},
                {a: 3, b: {c: 1}, c: 5},
            ]),
        );
    });

    it("$exists:true: non-generic entry", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.c": {$exists: true}, c: {$lt: 3}}, [
            {a: 3, b: {c: 1}, c: 2},
            {a: 3, b: {c: {nested: 1}}, c: 2},
            {a: 3, b: {c: {}}, c: 2},
            {a: 3, b: {c: null}, c: 2},
        ]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_bc_object);
    });

    it("$type:object: non-generic entry", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.c": {$type: "object"}, c: {$lt: 3}}, [
            {a: 3, b: {c: {nested: 1}}, c: 2},
            {a: 3, b: {c: {}}, c: 2},
        ]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_bc_object);
    });

    it("$eq:{}: non-generic entry", function () {
        // $eq:{} gives a tight bound on the exact empty-object value; no sub-path scan needed.
        const kNonGenericScan_bc_emptyObj = {
            indexName: kIndexName,
            keyPattern: {a: 1, "$_path": 1, "b.c": 1, c: 1},
            indexBounds: {
                a: ["[3.0, 3.0]"],
                "$_path": ['["b.c", "b.c"]'],
                "b.c": ["[{}, {}]"],
                c: ["[-inf, 3.0)"],
            },
        };
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.c": {$eq: {}}, c: {$lt: 3}}, [
            {a: 3, b: {c: {}}, c: 2},
        ]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_bc_emptyObj);
    });

    it("$gte:{}: generic entry", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.c": {$gte: {}}, c: {$lt: 3}}, [
            {a: 3, b: {c: {nested: 1}}, c: 2},
            {a: 3, b: {c: {}}, c: 2},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a3);
    });

    it("$eq:{nested:1}: generic entry", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.c": {$eq: {nested: 1}}, c: {$lt: 3}}, [
            {a: 3, b: {c: {nested: 1}}, c: 2},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a3);
    });
});

// $expr is slightly different - $expr:{$eq:null} matches only literal null, not missing, and does not type-bracket.
// We only use generic CWI expansion for this reason, even in cases where we could use non-generic CWI based on
// predicate constants.
describe("$expr predicate", function () {
    const coll = getTestCollection("expr");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, b: {x: 1}, c: 1},
                {a: 1, b: {x: -5}, c: 1},
                {a: 1, c: 1},
                {a: 1, b: {x: null}, c: 1},
                {a: 2, c: 1},
            ]),
        );
    });

    it("$expr eq null", function () {
        const exprPlans = assertResultsAndGetIxscans(coll, {a: 1, $expr: {$eq: ["$b.x", null]}, c: 1}, [
            {a: 1, b: {x: null}, c: 1},
        ]);
        assertAllPlansIxScan(exprPlans, kGenericScan_a1);

        const matchPlans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": null, c: 1}, [
            {a: 1, c: 1},
            {a: 1, b: {x: null}, c: 1},
        ]);
        assertAllPlansIxScan(matchPlans, kGenericScan_a1);
    });
    it("$expr lt: missing treated as null (null < threshold)", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, $expr: {$lt: ["$b.x", 0]}, c: 1}, [
            {a: 1, b: {x: -5}, c: 1},
            {a: 1, c: 1},
            {a: 1, b: {x: null}, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
    it("$expr gt: missing treated as null (null not > threshold)", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 1, $expr: {$gt: ["$b.x", 0]}, c: 1}, [
            {a: 1, b: {x: 1}, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
    });
});

describe("predicates matching missing/null + wildcard projection", function () {
    const coll = getTestCollection("proj_null_residual");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "$**": 1}, {wildcardProjection: {b: 1, c: 1}}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, c: 1},
                {a: 1, b: null, c: 1},
                {a: 1, b: 1, c: 1},
                {a: 9, c: 1},
            ]),
        );
    });

    it("null equality", function () {
        // b:null is ineligible for non-generic; the planner uses the non-generic entry for c
        // instead, with tight bounds on c=1 and $_path fixed to "c".
        const kNonGenericScan_c = {
            indexName: "a_1_$**_1",
            keyPattern: {a: 1, "$_path": 1, c: 1},
            indexBounds: {a: ["[1.0, 1.0]"], "$_path": ['["c", "c"]'], c: ["[1.0, 1.0]"]},
        };
        const plans = assertResultsAndGetIxscans(coll, {a: 1, b: null, c: 1}, [
            {a: 1, c: 1},
            {a: 1, b: null, c: 1},
        ]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_c);
    });
});

// With an unindexed field predicate we can use the CWI with generic $_path for prefix-only queries,
// or the non-generic 'b.x' entry for queries with a compatible predicate on the wildcard.
describe("IXSCAN with predicate on unindexed field", function () {
    const coll = getTestCollection("residual");
    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 3, b: {x: 1}, c: 7, d: "foo"},
                {a: 3, b: {x: 2}, c: 7, d: "foo"},
                {a: 3, b: {x: 1}, c: 7, d: "bar"},
                {a: 3, b: {x: 1}, c: 3, d: "foo"},
                {a: 9, b: {x: 1}, c: 7, d: "foo"},
                {a: 3, b: {}, c: 7, d: "foo"},
                {a: 3, b: {x: null}, c: 7, d: "foo"},
                {a: 3, b: null, c: 7, d: "foo"},
                {a: 3, c: 7, d: "foo"},
            ]),
        );
    });

    it("prefix-only with residual", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, c: {$gte: 5}, d: "foo"}, [
            {a: 3, b: {x: 1}, c: 7, d: "foo"},
            {a: 3, b: {x: 2}, c: 7, d: "foo"},
            {a: 3, b: {}, c: 7, d: "foo"},
            {a: 3, b: {x: null}, c: 7, d: "foo"},
            {a: 3, b: null, c: 7, d: "foo"},
            {a: 3, c: 7, d: "foo"},
        ]);
        // No wildcard predicate: prefix-only generic scan; c cannot be tightened.
        assertAllPlansIxScan(plans, kGenericScan_a3);
    });

    it("scalar predicate with residual", function () {
        // Non-generic entry: $_path fixed to "b.x", tight bounds on b.x=1 and c>=5.
        const kNonGenericScan_bx = {
            indexName: kIndexName,
            keyPattern: {a: 1, "$_path": 1, "b.x": 1, c: 1},
            indexBounds: {
                a: ["[3.0, 3.0]"],
                "$_path": ['["b.x", "b.x"]'],
                "b.x": ["[1.0, 1.0]"],
                c: ["[5.0, inf]"],
            },
        };
        const plans = assertResultsAndGetIxscans(coll, {a: 3, "b.x": 1, c: {$gte: 5}, d: "foo"}, [
            {a: 3, b: {x: 1}, c: 7, d: "foo"},
        ]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_bx);
    });

    // $expr{$eq} cannot give us narrower bounds on 'b.x'.
    it("$expr eq with residual", function () {
        const plans = assertResultsAndGetIxscans(coll, {a: 3, $expr: {$eq: ["$b.x", 1]}, c: {$gte: 5}, d: "foo"}, [
            {a: 3, b: {x: 1}, c: 7, d: "foo"},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a3);
    });
});

// With two predicates on the wildcard, one can be used with a non-generic path index bound, but not both.
describe("two wildcard predicates", function () {
    const coll = getTestCollection("two_wildcard_predicates");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, b: {x: 1, y: 2}, c: 1},
                {a: 1, b: {x: 1, y: 9}, c: 1},
                {a: 1, b: {x: 9, y: 2}, c: 1},
                {a: 2, b: {x: 1, y: 2}, c: 1},
                {a: 1, b: {x: null, y: null}, c: 1},
                {a: 1, b: {}, c: 1},
                {a: 1, b: null, c: 1},
                {a: 1, c: 1},
            ]),
        );
    });

    it("first uses non-generic entry", function () {
        // The planner picks one of b.x or b.y as the non-generic entry; the other is a residual.
        const kNonGenericScan_bx = {
            indexName: kIndexName,
            keyPattern: {a: 1, "$_path": 1, "b.x": 1, c: 1},
            indexBounds: {a: ["[1.0, 1.0]"], "$_path": ['["b.x", "b.x"]'], "b.x": ["[1.0, 1.0]"], c: ["[1.0, 1.0]"]},
        };
        const kNonGenericScan_by = {
            indexName: kIndexName,
            keyPattern: {a: 1, "$_path": 1, "b.y": 1, c: 1},
            indexBounds: {a: ["[1.0, 1.0]"], "$_path": ['["b.y", "b.y"]'], "b.y": ["[2.0, 2.0]"], c: ["[1.0, 1.0]"]},
        };
        const plans = assertResultsAndGetIxscans(coll, {a: 1, "b.x": 1, "b.y": 2, c: 1}, [
            {a: 1, b: {x: 1, y: 2}, c: 1},
        ]);
        const winningScans = plans[0];
        assert.eq(winningScans.length, 1, "expected exactly one IXSCAN in winning plan", {winningScans});
        const usesBx = winningScans[0].keyPattern.hasOwnProperty("b.x");
        assertAllPlansIxScan(plans.slice(0, 1), usesBx ? kNonGenericScan_bx : kNonGenericScan_by);
    });
});

describe("two wildcard predicates + wildcard projection", function () {
    const coll = getTestCollection("projection_residual");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "$**": 1}, {wildcardProjection: {b: 1, c: 1}}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, b: 1, c: 1},
                {a: 1, b: 1, c: 9},
                {a: 1, b: 9, c: 1},
                {a: 9, b: 1, c: 1},
                {a: 1, b: null, c: 1},
                {a: 1, c: 1},
            ]),
        );
    });

    it("first uses non-generic entry", function () {
        // Non-generic entry for "b" with tight bounds on b=1; c is a residual.
        const kNonGenericScan_b = {
            indexName: "a_1_$**_1",
            keyPattern: {a: 1, "$_path": 1, b: 1},
            indexBounds: {a: ["[1.0, 1.0]"], "$_path": ['["b", "b"]'], b: ["[1.0, 1.0]"]},
        };
        const plans = assertResultsAndGetIxscans(coll, {a: 1, b: 1, c: 1}, [{a: 1, b: 1, c: 1}]);
        assertAllPlansIxScan(plans.slice(0, 1), kNonGenericScan_b);
    });
});

describe("SUBPLAN with unindexed field", function () {
    const coll = getTestCollection("or_subplan");

    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(coll.createIndex({z: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 3, b: {x: 1}, c: 7, d: "match", z: 99},
                {a: 3, b: {x: 2}, c: 7, d: "match", z: 99},
                {a: 9, b: {x: 1}, c: 7, d: "match", z: 1},
                {a: 3, b: {x: null}, c: 7, d: "match", z: 5},
                {a: 3, b: null, c: 7, d: "match", z: 6},
                {a: 3, c: 7, d: "match", z: 7},
            ]),
        );
    });

    it("CWI scan is used", function () {
        const kCwiScan = {
            indexName: kIndexName,
            keyPattern: {a: 1, "$_path": 1, "b.x": 1, c: 1},
            indexBounds: {
                a: ["[3.0, 3.0]"],
                "$_path": ['["b.x", "b.x"]'],
                "b.x": ["[1.0, 1.0]"],
                c: ["[5.0, inf]"],
            },
        };
        const kZScan = {indexName: "z_1", keyPattern: {z: 1}, indexBounds: {z: ["[1.0, 1.0]"]}};

        const query = {
            $or: [{$and: [{a: 3}, {"b.x": 1}, {c: {$gte: 5}}, {d: "match"}]}, {z: 1}],
        };
        const plans = assertResultsAndGetIxscans(coll, query, [
            {a: 3, b: {x: 1}, c: 7, d: "match", z: 99},
            {a: 9, b: {x: 1}, c: 7, d: "match", z: 1},
        ]);
        const cwiScan = plans[0].find((s) => s.indexName === kIndexName);
        assert(cwiScan, "expected a CWI scan in the OR plan", {scans: plans[0]});
        assert.eq(cwiScan.keyPattern, kCwiScan.keyPattern, "keyPattern mismatch", {cwiScan});
        assert.eq(cwiScan.indexBounds, kCwiScan.indexBounds, "indexBounds mismatch", {cwiScan});
        const zScan = plans[0].find((s) => s.indexName === "z_1");
        assert(zScan, "expected a z_1 scan in the OR plan", {scans: plans[0]});
        assert.eq(zScan.indexBounds, kZScan.indexBounds, "z indexBounds mismatch", {zScan});
    });
});

describe("$or predicates on two wildcard fields", function () {
    const coll = getTestCollection("or_two_wildcard_subfields");
    before(function () {
        coll.drop();
        assert.commandWorked(coll.createIndex({a: 1, "b.$**": 1, c: 1}));
        assert.commandWorked(
            coll.insertMany([
                {a: 1, b: {f: 5}, c: 1},
                {a: 1, b: {d: 3}, c: 1},
                {a: 1, b: {f: 2, d: 3}, c: 1},
                {a: 1, b: {f: -1, d: -1}, c: 1},
                {a: 1, b: {x: 1}, c: 1},
                {a: 2, b: {f: 5}, c: 1},
                {a: 1, b: {f: 5}, c: 2},
                {a: 1, b: {f: null, d: null}, c: 1},
                {a: 1, b: null, c: 1},
                {a: 1, c: 1},
                {a: 1, b: {}, c: 1},
            ]),
        );
    });

    it("generic entry even when individually eligible", function () {
        // This uses the generic $_path expansion, although if we had two queries instead of $or we could use
        // non-generic expansion. We could do better theoretically, if we used a bound like [b.d, b.f].
        const query = {a: 1, c: 1, $or: [{"b.f": {$gte: 0}}, {"b.d": {$gte: 0}}]};
        const plans = assertResultsAndGetIxscans(coll, query, [
            {a: 1, b: {f: 5}, c: 1},
            {a: 1, b: {d: 3}, c: 1},
            {a: 1, b: {f: 2, d: 3}, c: 1},
        ]);
        assertAllPlansIxScan(plans, kGenericScan_a1);
        assert.eq(plans.length, 1, "no non-generic candidates should be generated", {plans});
    });
});
