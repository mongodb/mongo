/*
 * Tests that verify that queries using an index for sorting produce the same results as queries
 * that do collscan instead.
 * @tags: [
 *  # Uses $where.
 *  requires_scripting,
 *  requires_fcv_82,
 *  # Retrieving results using toArray may require a getMore command.
 *  requires_getmore,
 *  # Wildcard indexes are sparse by definition hence cannot be used to provide sorting.
 *  wildcard_indexes_incompatible,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {
    getPlanStage,
    getPlanStages,
    getWinningPlanFromExplain,
    isCollscan,
    isIndexOnly,
    isIxscan
} from "jstests/libs/query/analyze_plan.js";

if (!FeatureFlagUtil.isPresentAndEnabled(db, "PushdownFilterToIXScanWhenUsingIndexForSort")) {
    quit();
}
const collName = 'index_for_sort';
db[collName].drop();
const coll = db[collName];

const numDocs = 10;
const docs = [];
for (let i = 0; i < numDocs; i++) {
    docs.push({a: i, b: i, c: i});
    // now combine with nulls
    docs.push({a: null, b: i, c: i});
    docs.push({a: i, b: null, c: i});
    docs.push({a: i, b: i, c: null});
    docs.push({a: null, b: null, c: i});
    docs.push({a: null, b: i, c: null});
    docs.push({a: i, b: null, c: null});
    docs.push({a: null, b: null, c: null});
    // now combine with undefined
    docs.push({a: undefined, b: i, c: i});
    docs.push({a: i, b: undefined, c: i});
    docs.push({a: i, b: i, c: undefined});
    docs.push({a: undefined, b: undefined, c: i});
    docs.push({a: undefined, b: i, c: undefined});
    docs.push({a: i, b: undefined, c: undefined});
    docs.push({a: undefined, b: undefined, c: undefined});
    // Not adding this combinations since it will result in failures
    // IXSCAN returns {a: 1, b: null, c: null}
    // COLLSCAN returns {a: 1}
    // This is because of null/missing issues in our index format. See SERVER-12869
    // docs.push({b: i, c: i});
    // docs.push({a: i, c: i});
    // docs.push({a: i, b: i});
    // docs.push({b: i});
    // docs.push({a: i});
    // docs.push({c: i});
}
assert.commandWorked(coll.insertMany(docs));
const firstIndexKey = "a";
const secondIndexKey = "b";
const thirdIndexKey = "c";

assert.commandWorked(
    coll.createIndex({[firstIndexKey]: 1, [secondIndexKey]: 1, [thirdIndexKey]: 1}));

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
        predicate: {[secondIndexKey]: {$gt: 0}, [thirdIndexKey]: {$gt: 0}}
    },
    {
        covered: true,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$eq: 0}, [thirdIndexKey]: {$eq: 0}}
    },
    {
        covered: true,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$ne: 0}, [thirdIndexKey]: {$ne: 0}}
    },
    {
        covered: true,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$in: [0, 1]}, [thirdIndexKey]: {$in: [0, 1]}}
    },
    {
        covered: true,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$nin: [0, 1]}, [thirdIndexKey]: {$nin: [0, 1]}}
    },
    // AND predicates on second and not indexed fields. Predicate should be split between IXSCAN
    // and FETCH
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$gt: 0}, unindexedField: {$gt: 0}}
    },
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$eq: 0}, unindexedField: {$eq: 0}}
    },
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$ne: 0}, unindexedField: {$ne: 0}}
    },
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$in: [0, 1]}, unindexedField: {$in: [0, 1]}}
    },
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {[secondIndexKey]: {$nin: [0, 1]}, unindexedField: {$nin: [0, 1]}}
    },
    // OR predicates on second and not indexed fields. Predicate is not split and kept in FETCH
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {$or: [{[secondIndexKey]: {$gt: 0}}, {unindexedField: {$gt: 0}}]}
    },
    // NOR predicates on second and not indexed fields. Predicate is not split and kept in FETCH
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {$nor: [{[secondIndexKey]: {$gt: 0}}, {unindexedField: {$gt: 0}}]}
    },
    // elemMatch predicates on second field. Predicate is not split and kept in FETCH
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {[secondIndexKey]: {$elemMatch: {$gt: 0}}}
    },
    {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$type: "number"}}},
    {
        covered: false,
        filteredOnIndexStage: true,
        predicate: {$expr: {$gt: [`$${secondIndexKey}`, 0]}}
    },
    {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: /^1/}},
    {covered: true, filteredOnIndexStage: true, predicate: {[secondIndexKey]: {$mod: [2, 0]}}},
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {
            $where: function() {
                return this.b === 1;
            }
        }
    },
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {$expr: {$gt: [`$${secondIndexKey}`, {$rand: {}}]}}
    },
    {covered: false, filteredOnIndexStage: false, predicate: {[secondIndexKey]: {$size: 1}}},
    {
        covered: false,
        filteredOnIndexStage: false,
        predicate: {[secondIndexKey]: {$bitsAllSet: [1]}}
    },
];
predicates.forEach(({covered, predicate, filteredOnIndexStage}) => {
    jsTestLog(`Testing predicate: ${tojson(predicate)}. Covered: ${
        covered}. Filtered on index stage: ${filteredOnIndexStage}`);
    const query = () =>
        coll.find(predicate,
                  {[firstIndexKey]: 1, [secondIndexKey]: 1, [thirdIndexKey]: 1, "_id": 0})
            .sort({[firstIndexKey]: 1});
    // Ensure index is used for sorting.
    const indexScanPlan = query().explain();
    let winningPlan = getWinningPlanFromExplain(indexScanPlan.queryPlanner);
    if (FixtureHelpers.isSharded(coll)) {
        assert(isIxscan(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
        assert(!isIndexOnly(db, winningPlan),
               `Expected not only index scan ${tojson(indexScanPlan)}`);
        const fetchStages = getPlanStages(winningPlan, "FETCH");
        assert(fetchStages.length >= 1,
               `Expected at least one FETCH stages, got ${tojson(fetchStages)}`);
        if (covered) {
            // In sharded collections we cannot get a covered index scan unless filtering on the
            // shard key so a fetch is needed, but it'd be empty.
            assert(fetchStages.every((fetchStage) => !fetchStage.hasOwnProperty("filter")),
                   `Expected no filter in the FETCH stage, got ${tojson(fetchStages)}`);
        } else {
            assert(fetchStages.every((fetchStage) => fetchStage.hasOwnProperty("filter")),
                   `Expected filter in the FETCH stage, got ${tojson(fetchStages)}`);
        }
        const ixScanStages = getPlanStages(winningPlan, "IXSCAN");
        assert(ixScanStages.length >= 1,
               `Expected at least one IXSCAN stages, got ${tojson(ixScanStages)}`);
        if (filteredOnIndexStage) {
            assert(ixScanStages.every(
                (ixScanStage) => ixScanStage.hasOwnProperty("filter"),
                `Expected filter in the IXSCAN stage, got ${tojson(ixScanStages)}`));
        } else {
            assert(ixScanStages.every(
                (ixScanStage) => !ixScanStage.hasOwnProperty("filter"),
                `Expected no filter in the IXSCAN stage, got ${tojson(ixScanStages)}`));
        }
    } else {
        if (covered) {
            assert(isIndexOnly(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
        } else {
            assert(isIxscan(db, winningPlan), `Expected index scan ${tojson(indexScanPlan)}`);
            assert(!isIndexOnly(db, winningPlan),
                   `Expected not only index scan ${tojson(indexScanPlan)}`);
            const fetchStage = getPlanStage(winningPlan, "FETCH");
            assert(fetchStage.hasOwnProperty("filter"),
                   `Expected filter in the FETCH stage, got ${tojson(fetchStage)}`);
        }
        const ixScanStage = getPlanStage(winningPlan, "IXSCAN");
        if (filteredOnIndexStage) {
            assert(ixScanStage.hasOwnProperty("filter"),
                   `Expected filter in the IXSCAN stage, got ${tojson(ixScanStage)}`);
        } else {
            assert(!ixScanStage.hasOwnProperty("filter"),
                   `Expected no filter in the IXSCAN stage, got ${tojson(ixScanStage)}`);
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
    assert.sameMembers(indexScanResults,
                       collScanResults,
                       `Collscan results: ${tojson(collScanResults)}. \nIndexscan results: ${
                           tojson(indexScanResults)} \nIndexscan plan: ${
                           tojson(indexScanPlan)} \nCollscan plan: ${tojson(collScanPlan)}`);
});
