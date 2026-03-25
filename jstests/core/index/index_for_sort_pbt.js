/**
 * A property-based test that targets queries using an index scan to satisfy a sort.
 * It asserts that using the index and potentially pushing filters down to the IXSCAN stage
 * produces the same results as a COLLSCAN + SORT baseline.
 *
 * @tags: [
 *  requires_fcv_83,
 *  # Retrieving results using toArray may require a getMore command.
 *  requires_getmore,
 *  # Wildcard indexes are sparse by definition hence cannot be used to provide sorting.
 *  wildcard_indexes_incompatible,
 *  # Test creates specific indexes and assumes no implicit indexes are added.
 *  assumes_no_implicit_index_creation,
 * ]
 */

import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";

if (isSlowBuild(db)) {
    jsTest.log.info("Skipping index_for_sort_pbt on slow builds.");
    quit();
}

const originalKnobValue = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: 1}),
).internalQueryPlannerPushdownFilterToIxscanForSort;
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: true}));

try {
    const coll = db.index_for_sort_pbt;

    const numRuns = 400;

    const intValArb = fc.integer({min: -3, max: 3});
    const stringValArb = fc.stringOf(fc.constantFrom("a", "b", "c"), {minLength: 0, maxLength: 4});
    const intOrIntArrayArb = fc.oneof(intValArb, fc.array(intValArb, {minLength: 0, maxLength: 4}));

    // Our document schema:
    //
    // {
    //   a: int,
    //   b: int | [int, ...],
    //   c: int | [int, ...],
    //   nested: { x: int | [int,...], y: int },
    //   s: string
    // }
    //
    // All fields are always present (no missing fields), so $exists:true/false behave
    // consistently and we avoid SERVER-12869 (cannot distinguish null vs missing issues). Those scenarios are covered in index_for_sort.js test.
    const docArb = fc.record({
        a: intValArb,
        b: intOrIntArrayArb,
        c: intOrIntArrayArb,
        nested: fc.record({
            x: intOrIntArrayArb,
            y: intValArb,
        }),
        s: stringValArb,
    });

    const indexModelArb = fc.constantFrom(
        {a: 1, b: 1},
        {a: 1, b: -1},
        {a: 1, c: 1},
        {a: 1, "nested.x": 1},
        {a: 1, s: 1},
    );

    // ---------------------------
    // Predicate generators
    // ---------------------------

    function basicPredicate(field) {
        return fc.oneof(
            // Equalities.
            intValArb.map((v) => ({[field]: v})),
            intValArb.map((v) => ({[field]: {$eq: v}})),

            // Inequalities.
            intValArb.map((v) => ({[field]: {$gt: v}})),
            intValArb.map((v) => ({[field]: {$gte: v}})),
            intValArb.map((v) => ({[field]: {$lt: v}})),
            intValArb.map((v) => ({[field]: {$lte: v}})),

            // Simple closed range.
            fc.tuple(intValArb, intValArb).map(([v1, v2]) => {
                const lo = Math.min(v1, v2);
                const hi = Math.max(v1, v2);
                return {[field]: {$gte: lo, $lte: hi}};
            }),

            // Membership.
            fc.array(intValArb, {minLength: 1, maxLength: 4}).map((vals) => ({[field]: {$in: vals}})),
            fc.array(intValArb, {minLength: 1, maxLength: 4}).map((vals) => ({[field]: {$nin: vals}})),

            // $exists.
            fc.boolean().map((b) => ({[field]: {$exists: b}})),
        );
    }

    function combinedPredicates(field) {
        return fc.oneof(
            basicPredicate("").map((pred) => {
                const [[, innerExpr]] = Object.entries(pred);
                return {[field]: {$elemMatch: {$eq: innerExpr}}};
            }),

            fc.array(intValArb, {minLength: 1, maxLength: 3}).map((vals) => ({[field]: {$all: vals}})),
        );
    }

    function numericFieldPredicates(field) {
        return fc.oneof(basicPredicate(field), combinedPredicates(field));
    }

    function stringFieldPredicates(field) {
        return fc.oneof(
            // Equality and membership.
            stringValArb.map((v) => ({[field]: v})),
            stringValArb.map((v) => ({[field]: {$eq: v}})),
            fc.array(stringValArb, {minLength: 1, maxLength: 4}).map((vals) => ({[field]: {$in: vals}})),
            fc.array(stringValArb, {minLength: 1, maxLength: 4}).map((vals) => ({[field]: {$nin: vals}})),

            // $exists.
            fc.boolean().map((b) => ({[field]: {$exists: b}})),

            // Simple regexes over small alphabet; case-sensitive and case-insensitive.
            fc
                .constantFrom("a", "b", "c", "^a", "a$", "ab?")
                .chain((pat) => fc.constantFrom({[field]: {$regex: pat}}, {[field]: {$regex: pat, $options: "i"}})),
        );
    }

    const singleFieldFilterArb = fc.oneof(
        numericFieldPredicates("b"),
        numericFieldPredicates("c"),
        numericFieldPredicates("nested.x"),
        stringFieldPredicates("s"),
    );

    // Recursive filter arb: single predicate, or arbitrarily nested $and/$or/$nor.
    const {filter: filterArb} = fc.letrec((tie) => ({
        // A leaf is a single-field predicate.
        leaf: singleFieldFilterArb,

        // A compound filter nests 2–3 sub-filters under $and, $or, or $nor.
        compound: fc
            .tuple(fc.constantFrom("$and", "$or", "$nor"), fc.array(tie("filter"), {minLength: 2, maxLength: 3}))
            .map(([op, children]) => ({[op]: children})),

        // A filter is either a leaf (common) or a compound expression (rarer).
        // depthSize:"small" + maxDepth:3 keep trees shallow but allow nesting.
        filter: fc.oneof(
            {depthSize: "small", maxDepth: 3},
            {weight: 5, arbitrary: tie("leaf")},
            {weight: 1, arbitrary: tie("compound")},
        ),
    }));

    // ---------------------------
    // Workload model
    // ---------------------------

    const sortDirArb = fc.constantFrom(1, -1);

    const includeSecondKeyArb = fc.boolean();

    // Projection mode: no projection, only indexed fields (covered), or include a non-indexed field.
    const projectionModeArb = fc.constantFrom("none", "covered", "non-covered");

    const workloadArb = fc
        .record({
            docs: fc.array(docArb, {minLength: 1, maxLength: 60}),
            indexModel: indexModelArb,
            filter: filterArb,
            sortDir: sortDirArb,
            includeSecondKey: includeSecondKeyArb,
            projectionMode: projectionModeArb,
        })
        .map(({docs, indexModel, filter, sortDir, includeSecondKey, projectionMode}) => {
            const indexKey = indexModel;
            const sortSpec = {};
            const keys = Object.keys(indexKey);

            // Always sort on the leading field so the index can provide the sort.
            sortSpec[keys[0]] = sortDir;

            // Optionally sort on the second field as well.
            if (includeSecondKey && keys.length > 1) {
                sortSpec[keys[1]] = sortDir;
            }

            // Always exclude _id so that ties in sort key don't cause non-deterministic ordering.
            let projection = {_id: 0};
            if (projectionMode === "covered") {
                // Project only indexed fields (potential covered query).
                for (const k of keys) {
                    projection[k] = 1;
                }
            } else if (projectionMode === "non-covered") {
                // Include a non-indexed field to ensure a FETCH is needed.
                projection.a = 1;
                projection.s = 1;
            }

            // Assign unique 'a' values so the sort order is fully determined (no ties).
            // The leading sort key is always 'a', so uniqueness guarantees a stable ordering.
            docs.forEach((doc, i) => {
                doc.a = i;
            });

            return {docs, indexKey, filter, sortSpec, projection};
        });

    function runOne({docs, indexKey, filter, sortSpec, projection}) {
        coll.drop();
        if (docs.length > 0) {
            assert.commandWorked(coll.insertMany(docs));
        }

        assert.commandWorked(coll.createIndex(indexKey));

        const baseline = coll.find(filter, projection).sort(sortSpec).hint({$natural: 1}).toArray();
        const viaIndex = coll.find(filter, projection).sort(sortSpec).hint(indexKey).toArray();

        assert.eq(baseline, viaIndex);
    }

    fc.assert(fc.property(workloadArb, runOne), {numRuns});
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: originalKnobValue}),
    );
}
