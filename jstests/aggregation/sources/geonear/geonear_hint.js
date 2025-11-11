// Verify that an aggregate with $geoNear always uses the index hinted to the aggregate command.
// @tags: [
//  does_not_support_repeated_reads,
// ]

import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const collName = jsTest.name();
const coll = db[collName];
coll.drop();

// Save the names of each hinted index to use when examining the output of $indexStats.
const simpleGeoNearIndexName = "simpleGeoNearIndex";
const compoundGeoNearIndexName = "compoundGeoNearIndex";
const compoundGeoNearIndexNameLocationLast = "compoundGeoNearIndexLocationLast";
const nonGeoNearIndexName = "nonGeoNearIndex";

assert.commandWorked(coll.insert({_id: "1", state: "ACTIVE", location: [106, 10], name: "TEST"}));
assert.commandWorked(coll.createIndex({_id: 1, location: "2dsphere", state: 1}, {name: compoundGeoNearIndexName}));
assert.commandWorked(
    coll.createIndex({_id: 1, state: 1, location: "2dsphere"}, {name: compoundGeoNearIndexNameLocationLast}),
);
assert.commandWorked(coll.createIndex({location: "2dsphere"}, {name: simpleGeoNearIndexName}));
assert.commandWorked(coll.createIndex({location: 1}, {name: nonGeoNearIndexName}));

function makeGeoNearStage(query = {}) {
    return {
        $geoNear: {
            query: query,
            spherical: true,
            near: {coordinates: [106.65589, 10.787627], type: "Point"},
            distanceField: "distance",
            key: "location",
        },
    };
}

function checkInvalidHint(geoNearStage, hintIndexName) {
    assert.commandFailedWithCode(
        db.runCommand({
            aggregate: collName,
            pipeline: [geoNearStage],
            cursor: {},
            hint: hintIndexName,
        }),
        ErrorCodes.NoQueryExecutionPlans,
    );
}

// A compound index cannot be hinted when $geoNear doesn't filter on the first element.
checkInvalidHint(makeGeoNearStage(), compoundGeoNearIndexName);
checkInvalidHint(makeGeoNearStage({state: "ACTIVE"}), compoundGeoNearIndexName);
checkInvalidHint(makeGeoNearStage(), compoundGeoNearIndexNameLocationLast);
checkInvalidHint(makeGeoNearStage({state: "ACTIVE"}), compoundGeoNearIndexNameLocationLast);

// A non-geo index cannot be hinted for a $geoNear stage.
checkInvalidHint(makeGeoNearStage(), nonGeoNearIndexName);

const pipeline = [
    makeGeoNearStage({_id: {$in: ["1", "2", "3"]}, state: "ACTIVE"}),
    {$project: {id: 1, name: 1, distance: 1}},
];

// Run the aggregate with two different hinted indexes and confirm that the hinted index gets
// chosen each time. By testing that two separate index hints get used, we confirm that the
// index in each query plan gets chosen because it was hinted to the aggregate command and not
// because the query planner chose it over other indexes.
for (const indexHint of [simpleGeoNearIndexName, compoundGeoNearIndexName]) {
    // Run the aggregate with the hint and ensure that it worked.
    const aggResult = coll.aggregate(pipeline, {hint: indexHint});
    assert.eq(1, aggResult.itcount());

    const explain = coll.explain("executionStats").aggregate(pipeline, {hint: indexHint});
    const winningPlan = getWinningPlanFromExplain(explain);
    for (let indexScan of getPlanStages(winningPlan, "IXSCAN")) {
        assert.eq(indexScan.indexName, indexHint, indexScan);
    }

    // Run $indexStats and limit the results to the hinted index.
    const indexStats = coll.aggregate([{$indexStats: {}}, {$match: {name: indexHint}}]).toArray();

    // Verify that the hinted index was used exactly once. Note that in the case of a sharded
    // cluster, $indexStats will report per-shard stats, so we loop over the array returned by
    // the aggregate to check the stats for each machine.
    for (const indexEntry of indexStats) {
        assert(indexEntry.hasOwnProperty("accesses"), indexStats);
        const indexAccesses = indexEntry["accesses"];
        assert(indexAccesses.hasOwnProperty("ops"), indexStats);
        assert.gte(indexAccesses["ops"], 1, indexStats);
    }
}
