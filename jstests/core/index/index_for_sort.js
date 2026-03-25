/*
 * Tests that verify that queries using an index for sorting produce the same results as queries
 * that do collscan instead.
 * @tags: [
 *  # Uses $where.
 *  requires_scripting,
 *  requires_fcv_83,
 *  # Retrieving results using toArray may require a getMore command.
 *  requires_getmore,
 *  # Wildcard indexes are sparse by definition hence cannot be used to provide sorting.
 *  wildcard_indexes_incompatible,
 *  # Test creates specific indexes and assumes no implicit indexes are added.
 *  assumes_no_implicit_index_creation,
 * ]
 */

import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getPlanStage,
    getPlanStages,
    getWinningPlanFromExplain,
    isCollscan,
    isIndexOnly,
    isIxscan,
} from "jstests/libs/query/analyze_plan.js";

const originalKnobValue = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: 1}),
).internalQueryPlannerPushdownFilterToIxscanForSort;
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: true}));

try {
    const collName = "index_for_sort";
    db[collName].drop();
    const coll = db[collName];

    const docs = [];
    {
        docs.push({a: 0, b: 0, c: 0});
        // now combine with nulls
        docs.push({a: null, b: 0, c: 0});
        docs.push({a: 0, b: null, c: 0});
        docs.push({a: 0, b: 0, c: null});
        docs.push({a: null, b: null, c: 0});
        docs.push({a: null, b: 0, c: null});
        docs.push({a: 0, b: null, c: null});
        docs.push({a: null, b: null, c: null});
        // now combine with undefined
        docs.push({a: undefined, b: 0, c: 0});
        docs.push({a: 0, b: undefined, c: 0});
        docs.push({a: 0, b: 0, c: undefined});
        docs.push({a: undefined, b: undefined, c: 0});
        docs.push({a: undefined, b: 0, c: undefined});
        docs.push({a: 0, b: undefined, c: undefined});
        docs.push({a: undefined, b: undefined, c: undefined});
        // Not adding this combinations since it will result in failures
        // IXSCAN returns {a: 1, b: null, c: null}
        // COLLSCAN returns {a: 1}
        // This is because of null/missing issues in our index format. See SERVER-12869
        // TODO SERVER-12869. Uncomment this insertions once indexes can distinguish between non existing fields and null ones
        // docs.push({b: 0, c: 0});
        // docs.push({a: 0, c: 0});
        // docs.push({a: 0, b: 0});
        // docs.push({b: 0});
        // docs.push({a: 0});
        // docs.push({c: 0});
    }
    assert.commandWorked(coll.insertMany(docs));
    const firstIndexKey = "a";
    const secondIndexKey = "b";
    const thirdIndexKey = "c";

    assert.commandWorked(coll.createIndex({[firstIndexKey]: 1, [secondIndexKey]: 1, [thirdIndexKey]: 1}));

    const predicates = [
        {covered: true, filteredOnIndexStage: false, predicate: {}},
        // Predicates on first field. Should be translated to index bounds
        {covered: true, filteredOnIndexStage: false, predicate: {[firstIndexKey]: {$gt: 0}}},
        {covered: true, filteredOnIndexStage: false, predicate: {[firstIndexKey]: {$eq: 0}}},
        {covered: true, filteredOnIndexStage: false, predicate: {[firstIndexKey]: {$ne: 0}}},
        {covered: true, filteredOnIndexStage: false, predicate: {[firstIndexKey]: {$in: [0, 1]}}},
        {covered: true, filteredOnIndexStage: false, predicate: {[firstIndexKey]: {$nin: [0, 1]}}},
        // Predicates on second field. Should be treated as a filter within the IXSCAN stage
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$gt: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$eq: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$ne: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$in: [0, 1]}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$nin: [0, 1]}}},
        // Predicates on a third field. Should be treated as a filter within the IXSCAN stage
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$gt: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$eq: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$ne: 0}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$in: [0, 1]}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$nin: [0, 1]}}},
        // AND Predicates on both second and third fields. Should be treated as a filter within the
        // IXSCAN stage
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$gt: 0}, [thirdIndexKey]: {$gt: 0}},
        },
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$eq: 0}, [thirdIndexKey]: {$eq: 0}},
        },
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$ne: 0}, [thirdIndexKey]: {$ne: 0}},
        },
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$in: [0, 1]}, [thirdIndexKey]: {$in: [0, 1]}},
        },
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$nin: [0, 1]}, [thirdIndexKey]: {$nin: [0, 1]}},
        },
        // AND predicates on second and not indexed fields. Predicate should be split between IXSCAN
        // and FETCH
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$gt: 0}, unindexedField: {$gt: 0}},
        },
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$eq: 0}, unindexedField: {$eq: 0}},
        },
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$ne: 0}, unindexedField: {$ne: 0}},
        },
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$in: [0, 1]}, unindexedField: {$in: [0, 1]}},
        },
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$nin: [0, 1]}, unindexedField: {$nin: [0, 1]}},
        },
        // OR predicates on second and not indexed fields. Predicate is not split and kept in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {$or: [{[secondIndexKey]: {$gt: 0}}, {unindexedField: {$gt: 0}}]},
        },
        // NOR predicates on second and not indexed fields. Predicate is not split and kept in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {$nor: [{[secondIndexKey]: {$gt: 0}}, {unindexedField: {$gt: 0}}]},
        },
        // elemMatch predicates on second field. Predicate is not split and kept in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$elemMatch: {$gt: 0}}},
        },
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$type: "number"}}},
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {$expr: {$gt: [`$${secondIndexKey}`, 0]}},
        },
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: /^1/}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$mod: [2, 0]}}},
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {
                $where: function () {
                    return this.b === 1;
                },
            },
        },
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {$expr: {$gt: [`$${secondIndexKey}`, {$rand: {}}]}},
        },
        {covered: false, filteredOnIndexStage: false, predicate: {[secondIndexKey]: {$size: 1}}},
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$bitsAllSet: [1]}},
        },
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$exists: true}},
        },
        // NOT-wrapped indexed-safe predicates: pushed to IXSCAN
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$not: {$gt: 0}}}},
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$not: {$lt: 0}}}},
        // NOT-wrapped index-unsafe predicates: stay in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$not: {$type: 10}}},
        },
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$not: {$exists: true}}},
        },
        // AND of indexed + $exists (split: indexed → IXSCAN, $exists → FETCH)
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$eq: 0}, [thirdIndexKey]: {$exists: true}},
        },
        // AND of indexed + $type:null (split: indexed → IXSCAN, $type:null → FETCH)
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$eq: 0}, [thirdIndexKey]: {$type: 10}},
        },
        // AND of $ne (indexed-safe) + NOT($exists) (unsafe) → split
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$ne: 0}, [thirdIndexKey]: {$not: {$exists: true}}},
        },
        // AND of multiple $ne on indexed fields → all pushed to IXSCAN
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$ne: 0}, [thirdIndexKey]: {$ne: 0}},
        },
        // Range predicate $gt + $lt on same indexed field → pushed to IXSCAN
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$gt: -1, $lt: 1}},
        },
        // OR with all indexed children inside AND → OR pushed to IXSCAN (after bug fix)
        {
            covered: true,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: 0, $or: [{[secondIndexKey]: 1}, {[thirdIndexKey]: 1}]},
        },
        // OR with all unindexed children inside AND → OR stays in FETCH, indexed pushed to IXSCAN
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {[secondIndexKey]: {$eq: 0}, $or: [{unindexedField: 0}, {anotherUnindexed: 1}]},
        },
        // Multiple unsafe predicates together → nothing pushed, all in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {[secondIndexKey]: {$exists: true}, [thirdIndexKey]: {$type: 10}},
        },
        // NOR inside AND with indexed children → canonicalized to AND(NOT, NOT), pushed to IXSCAN
        {
            covered: false,
            filteredOnIndexStage: true,
            predicate: {unindexedField: 0, $nor: [{[secondIndexKey]: 0}, {[thirdIndexKey]: 0}]},
        },
        // $nin on indexed field → equivalent to NOT($in), pushed to IXSCAN
        {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$nin: [0, 1]}}},
        // Only unindexed predicates → nothing pushed, all in FETCH
        {covered: false, filteredOnIndexStage: false, predicate: {unindexedField: {$eq: 0}}},
        {covered: false, filteredOnIndexStage: false, predicate: {unindexedField: {$ne: 0}}},
        // AND of only unindexed predicates → nothing pushed, all in FETCH
        {
            covered: false,
            filteredOnIndexStage: false,
            predicate: {unindexedField: {$eq: 0}, anotherUnindexed: {$gt: 0}},
        },
        // $type: "string" (non-null type) on indexed field → safe, pushed to IXSCAN
        {covered: true, filteredOnIndexStage: true, predicate: {[thirdIndexKey]: {$type: "number"}}},
    ];
    predicates.forEach(({covered, predicate, filteredOnIndexStage}) => {
        jsTest.log.info(
            `Testing predicate: ${tojson(predicate)}. Covered: ${
                covered
            }. Filtered on index stage: ${filteredOnIndexStage}`,
        );
        const query = () =>
            coll
                .find(predicate, {[firstIndexKey]: 1, [secondIndexKey]: 1, [thirdIndexKey]: 1, "_id": 0})
                .sort({[firstIndexKey]: 1});
        // Ensure index is used for sorting.
        const indexScanPlan = query().explain();
        let winningPlan = getWinningPlanFromExplain(indexScanPlan.queryPlanner);
        if (FixtureHelpers.isSharded(coll)) {
            assert(isIxscan(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
            assert(!isIndexOnly(db, winningPlan), `Expected not only index scan ${tojson(indexScanPlan)}`);
            const fetchStages = getPlanStages(winningPlan, "FETCH");
            assert(fetchStages.length >= 1, `Expected at least one FETCH stages, got ${tojson(fetchStages)}`);
            if (covered) {
                // In sharded collections we cannot get a covered index scan unless filtering on the
                // shard key so a fetch is needed, but it'd be empty.
                assert(
                    fetchStages.every((fetchStage) => !fetchStage.hasOwnProperty("filter")),
                    `Expected no filter in the FETCH stage, got ${tojson(fetchStages)}`,
                );
            } else {
                assert(
                    fetchStages.every((fetchStage) => fetchStage.hasOwnProperty("filter")),
                    `Expected filter in the FETCH stage, got ${tojson(fetchStages)}`,
                );
            }
            const ixScanStages = getPlanStages(winningPlan, "IXSCAN");
            assert(ixScanStages.length >= 1, `Expected at least one IXSCAN stages, got ${tojson(ixScanStages)}`);
            if (filteredOnIndexStage) {
                assert(
                    ixScanStages.every(
                        (ixScanStage) => ixScanStage.hasOwnProperty("filter"),
                        `Expected filter in the IXSCAN stage, got ${tojson(ixScanStages)}`,
                    ),
                );
            } else {
                assert(
                    ixScanStages.every(
                        (ixScanStage) => !ixScanStage.hasOwnProperty("filter"),
                        `Expected no filter in the IXSCAN stage, got ${tojson(ixScanStages)}`,
                    ),
                );
            }
        } else {
            if (covered) {
                assert(isIndexOnly(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
            } else {
                assert(isIxscan(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
                assert(!isIndexOnly(db, winningPlan), `Expected not only index scan ${tojson(indexScanPlan)}`);
                const fetchStage = getPlanStage(winningPlan, "FETCH");
                assert(
                    fetchStage.hasOwnProperty("filter"),
                    `Expected filter in the FETCH stage, got ${tojson(fetchStage)}`,
                );
            }
            const ixScanStage = getPlanStage(winningPlan, "IXSCAN");
            if (filteredOnIndexStage) {
                assert(
                    ixScanStage.hasOwnProperty("filter"),
                    `Expected filter in the IXSCAN stage, got ${tojson(ixScanStage)}`,
                );
            } else {
                assert(
                    !ixScanStage.hasOwnProperty("filter"),
                    `Expected no filter in the IXSCAN stage, got ${tojson(ixScanStage)}`,
                );
            }
        }
        // Ensure collscan is used when hinted.
        const collScanPlan = query().hint({"$natural": 1}).explain();
        winningPlan = getWinningPlanFromExplain(collScanPlan.queryPlanner);
        assert(isCollscan(db, winningPlan), `Expected collection scan ${tojson(collScanPlan)}`);

        // Compare results
        const indexScanResults = query().toArray();
        const collScanResults = query().hint({"$natural": 1}).toArray();
        assert.eq(indexScanResults.length, collScanResults.length);
        assert.sameMembers(
            indexScanResults,
            collScanResults,
            `Collscan results: ${tojson(collScanResults)}. \nIndexscan results: ${tojson(
                indexScanResults,
            )} \nIndexscan plan: ${tojson(
                indexScanPlan,
            )} \nCollscan plan: ${tojson(collScanPlan)}. Difference: ${tojson(new Set(indexScanResults).difference(new Set(collScanResults)))}`,
        );
    });

    // --- Targeted correctness tests with specific document sets ---
    // These tests insert controlled documents and verify that pushed-down filters produce the same
    // results as a collection scan, catching cases where predicates might be silently dropped or
    // incorrectly evaluated at the IXSCAN stage.

    /**
     * Drops and recreates `coll` with the given index and documents, then asserts that the query
     * produces identical results whether executed via an index scan or a forced collection scan.
     *
     * @param {object[]} docs           - Documents to insert.
     * @param {object}   index          - Index key pattern to create.
     * @param {object}   query          - Query filter.
     * @param {object}   projection     - Projection to apply.
     * @param {string}   description    - Label used in failure messages.
     * @param {object}   [indexOptions] - Options forwarded to createIndex (e.g. sparse, partial).
     * @param {boolean}  [assertSameMembers=true] - When false, only compares result lengths (used for
     *                                   cases affected by SERVER-12869 null/missing ambiguity).
     */
    function runCorrectnessTest({
        docs,
        index,
        query,
        projection,
        description,
        indexOptions = {},
        assertSameMembers = true,
    }) {
        coll.drop();
        assert.commandWorked(coll.createIndex(index, indexOptions));
        assert.commandWorked(coll.insertMany(docs));
        const ixResults = coll.find(query, projection).sort({a: 1}).toArray();
        const csResults = coll.find(query, projection).sort({a: 1}).hint({$natural: 1}).toArray();
        if (assertSameMembers) {
            assert.sameMembers(
                ixResults,
                csResults,
                `${description} mismatch: IX=${tojson(ixResults)} CS=${tojson(csResults)}`,
            );
        } else {
            assert.eq(
                ixResults.length,
                csResults.length,
                `${description} length mismatch: IX=${tojson(ixResults)} CS=${tojson(csResults)}`,
            );
        }
    }

    runCorrectnessTest({
        description: "$exists:true on missing field",
        docs: [{a: 1}],
        index: {a: 1, b: 1},
        query: {b: {$exists: true}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "not $exists:true on missing field",
        docs: [{a: 1}],
        index: {a: 1, b: 1},
        query: {b: {$not: {$exists: true}}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$type:10 (null) on missing field",
        docs: [{a: 1}],
        index: {a: 1, b: 1},
        query: {b: {$type: 10}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$not $type:10 on missing field",
        docs: [{a: 1}],
        index: {a: 1, b: 1},
        query: {b: {$not: {$type: 10}}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "OR with all indexed children inside AND",
        docs: [
            {a: 1, b: 1, c: 1}, // matches b:1 AND (b:2 OR c:1) → yes
            {a: 2, b: 1, c: 2}, // matches b:1 but NOT (b:2 OR c:1) → no
            {a: 3, b: 2, c: 5}, // matches OR but not b:1 → no
            {a: 4, b: 1, c: 5}, // matches b:1 but NOT (b:2 OR c:1) → no
        ],
        index: {a: 1, b: 1, c: 1},
        query: {b: 1, $or: [{b: 2}, {c: 1}]},
        projection: {a: 1, b: 1, c: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "NOT($gt) pushed to IXSCAN",
        docs: [
            {a: 1, b: 0},
            {a: 2, b: 5},
            {a: 3, b: 10},
            {a: 4, b: -1},
        ],
        index: {a: 1, b: 1},
        query: {b: {$not: {$gt: 5}}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "AND of $ne (safe) + $exists (unsafe) splits correctly",
        docs: [
            {a: 1, b: 0, c: 1}, // b!=0? no → excluded
            {a: 2, b: 1, c: 1}, // b!=0? yes, c exists? yes → match
            {a: 3, b: 1}, // b!=0? yes, c exists? no → excluded
            {a: 4, b: 2, c: null}, // b!=0? yes, c exists? yes (null counts) → match
        ],
        index: {a: 1, b: 1, c: 1},
        query: {b: {$ne: 0}, c: {$exists: true}},
        projection: {a: 1, b: 1, c: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "NOR inside AND (canonicalized to AND(NOT, NOT))",
        docs: [
            {a: 1, b: 1, c: 1, j: 1}, // b==1 → nor excludes → no
            {a: 2, b: 2, c: 2, j: 1}, // b!=1, c!=1, j:1 → match
            {a: 3, b: 3, c: 1, j: 1}, // c==1 → nor excludes → no
            {a: 4, b: 4, c: 4, j: 2}, // j!=1 → no
            {a: 5, b: 5, c: 5, j: 1}, // b!=1, c!=1, j:1 → match
        ],
        index: {a: 1, b: 1, c: 1},
        query: {j: 1, $nor: [{b: 1}, {c: 1}]},
        projection: {a: 1, b: 1, c: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "OR with mixed indexed/unindexed children stays in FETCH",
        docs: [
            {a: 1, b: 1, j: 5}, // b:1 AND (b:2 OR j:5) → j:5 satisfies OR → yes
            {a: 2, b: 1, j: 1}, // b:1 AND (b:2 OR j:5) → OR not satisfied → no
            {a: 3, b: 2, j: 1}, // b!=1 → no
            {a: 4, b: 1, j: 0}, // b:1 but OR not satisfied → no
        ],
        index: {a: 1, b: 1},
        query: {b: 1, $or: [{b: 2}, {j: 5}]},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$type:null stays in FETCH",
        docs: [
            {a: 1, b: null},
            {a: 2, b: 1},
            {a: 3}, // b is missing (not null)
        ],
        index: {a: 1, b: 1},
        query: {b: {$type: 10}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$exists:true stays in FETCH",
        docs: [
            {a: 1, b: 1},
            {a: 2, b: null}, // b is null but exists
            {a: 3}, // b is missing → $exists:true returns false
        ],
        index: {a: 1, b: 1},
        query: {b: {$exists: true}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$nin on indexed field pushed to IXSCAN",
        docs: [
            {a: 1, b: 1},
            {a: 2, b: 2},
            {a: 3, b: 3},
            {a: 4, b: 4},
        ],
        index: {a: 1, b: 1},
        query: {b: {$nin: [1, 3]}},
        projection: {a: 1, b: 1, _id: 0},
    });

    // TODO SERVER-12869: The two tests below only compare result lengths (not members) because the
    // IXSCAN returns {a:3, b:null} for a missing-field document instead of {a:3} as the collscan
    // does, due to null/missing ambiguity in the index format.
    runCorrectnessTest({
        description: "$eq null length check",
        docs: [
            {a: 1, b: 1},
            {a: 2, b: null}, // b is null but exists
            {a: 3}, // b is missing
        ],
        index: {a: 1, b: 1},
        query: {b: null},
        projection: {a: 1, b: 1, _id: 0},
        assertSameMembers: false,
    });

    runCorrectnessTest({
        description: "$ne null on indexed field",
        docs: [
            {a: 1, b: 1},
            {a: 2, b: null}, // b is null but exists
            {a: 3}, // b is missing
        ],
        index: {a: 1, b: 1},
        query: {b: {$ne: null}},
        projection: {a: 1, b: 1, _id: 0},
    });

    runCorrectnessTest({
        description: "$in null length check",
        docs: [
            {a: 1, b: 1},
            {a: 2, b: null}, // b is null but exists
            {a: 3}, // b is missing
        ],
        index: {a: 1, b: 1},
        query: {b: {$in: [null]}},
        projection: {a: 1, b: 1, _id: 0},
        assertSameMembers: false,
    });

    // --- Tests verifying the optimization is NOT applied for special index types ---
    // Each case sets up its own collection, runs an explain, and asserts that no filter was
    // pushed down to the IXSCAN stage (if the planner chose an IXSCAN at all).

    const specialIndexCases = [
        {
            description: "multikey index",
            docs: [
                {a: 1, b: [10, 20]}, // b is an array → makes the index multikey
                {a: 2, b: [30, 40]},
                {a: 3, b: 50},
            ],
            index: {a: 1, b: 1},
            query: {b: {$gt: 15}},
            projection: {a: 1, b: 1, _id: 0},
            // In sharded mode docs are spread across shards; a shard receiving only scalar b values
            // will have a non-multikey index and may correctly push the filter. Only assert on
            // IXSCAN stages that are actually multikey.
            shardedIxscanFilter: (s) => s.isMultiKey,
        },
        {
            description: "partial index",
            docs: [
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: -1, b: 3}, // not indexed (a <= 0)
            ],
            index: {a: 1, b: 1},
            indexOptions: {partialFilterExpression: {a: {$gt: 0}}},
            query: {b: {$gt: 0}, a: {$gt: 0}},
            projection: {a: 1, b: 1, _id: 0},
        },
        {
            description: "sparse index",
            docs: [
                {a: 1, b: 1},
                {a: 2, b: 2},
                {a: 3}, // missing 'b', not indexed by the sparse index
            ],
            index: {a: 1, b: 1},
            indexOptions: {sparse: true},
            query: {b: {$gt: 0}},
            projection: {a: 1, b: 1, _id: 0},
        },
    ];

    specialIndexCases.forEach(
        ({description, docs, index, indexOptions = {}, query, projection, shardedIxscanFilter}) => {
            coll.drop();
            assert.commandWorked(coll.createIndex(index, indexOptions));
            assert.commandWorked(coll.insertMany(docs));
            const plan = getWinningPlanFromExplain(coll.find(query, projection).sort({a: 1}).explain().queryPlanner);
            if (FixtureHelpers.isSharded(coll)) {
                const ixScanStages = getPlanStages(plan, "IXSCAN");
                const stagesToCheck = shardedIxscanFilter ? ixScanStages.filter(shardedIxscanFilter) : ixScanStages;
                assert(
                    stagesToCheck.every((s) => !s.hasOwnProperty("filter")),
                    `Expected no filter on IXSCAN for ${description}: ${tojson(stagesToCheck)}`,
                );
            } else {
                const ixScanStage = getPlanStage(plan, "IXSCAN");
                if (ixScanStage) {
                    assert(
                        !ixScanStage.hasOwnProperty("filter"),
                        `Expected no filter on IXSCAN for ${description}: ${tojson(ixScanStage)}`,
                    );
                }
            }
        },
    );
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryPlannerPushdownFilterToIxscanForSort: originalKnobValue}),
    );
}
