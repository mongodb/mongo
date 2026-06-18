/**
 * Reproduces SERVER-105903.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_index_creation,
 *   exclude_from_timeseries_crud_passthrough,
 * ]
 */

import {getWinningPlanFromExplain, planHasStage} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
coll.drop();

const docs = [{_id: 1}];
assert.commandWorked(coll.insert(docs));

assert.commandWorked(
    coll.createIndex({
        a: 1,
        b: 1,
    }),
);

const explain = coll
    .find({
        "$and": [{a: 1}, {a: 1}],
    })
    .sort({b: 1})
    .explain();

jsTest.log.info("Explain output", {explain});

const winningPlan = getWinningPlanFromExplain(explain);
const hasSort = planHasStage(db, winningPlan, "SORT");

// We expect that we can use a forward scan of the index to satisfy the sort without needing a
// blocking SORT stage.
assert(!hasSort, "Expected no SORT stage in the winning plan, but found one.");
