// Verify that an aggregate with $geoNear always uses the index hinted to the aggregate command.
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";

const collName = jsTest.name();
const coll = db[collName];
coll.drop();

// Save the names of each hinted index to use when examining the output of $indexStats.
const simpleGeoNearIndexName = "simpleGeoNearIndex";
const compoundGeoNearIndexName = "compoundGeoNearIndex";

assert.commandWorked(coll.insert({_id: "1", state: "ACTIVE", location: [106, 10], name: "TEST"}));
assert.commandWorked(
    coll.createIndex({_id: 1, location: "2dsphere", state: 1}, {name: compoundGeoNearIndexName}));
assert.commandWorked(coll.createIndex({location: "2dsphere"}, {name: simpleGeoNearIndexName}));

const pipeline = [
    {
        $geoNear: {
            query: {_id: {$in: ["1", "2", "3"]}, state: "ACTIVE"},
            spherical: true,
            near: {coordinates: [106.65589, 10.787627], type: "Point"},
            distanceField: "distance",
            key: "location"
        }
    },
    {$project: {id: 1, name: 1, distance: 1}}
];

// Run the aggregate with two different hinted indexes and confirm that the hinted index gets
// chosen each time. By testing that two separate index hints get used, we confirm that the
// index in each query plan gets chosen because it was hinted to the aggregate command and not
// because the query planner chose it over other indexes.
for (const indexHint of [simpleGeoNearIndexName, compoundGeoNearIndexName]) {
    // Run the aggregate with the hint and ensure that it worked.
    const aggResult = coll.aggregate(pipeline, {hint: indexHint});
    assert.eq(1, aggResult.itcount());

    // Run $indexStats and limit the results to the hinted index.
    const indexStats = coll.aggregate([{$indexStats: {}}, {$match: {name: indexHint}}]).toArray();

    // Verify that the hinted index was used exactly once. Note that in the case of a sharded
    // cluster, $indexStats will report per-shard stats, so we loop over the array returned by
    // the aggregate to check the stats for each machine.
    for (const indexEntry of indexStats) {
        assert(indexEntry.hasOwnProperty("accesses"), indexStats);
        const indexAccesses = indexEntry["accesses"];
        assert(indexAccesses.hasOwnProperty("ops"), indexStats);
        assert.eq(1, indexAccesses["ops"], indexStats);
    }
}
}());
