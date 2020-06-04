// Verify that an aggregate with $geoNear always uses the index hinted to the aggregate command.
(function() {
    "use strict";

    const collName = jsTest.name();
    const coll = db[collName];
    coll.drop();

    // Save the names along with keyPatterns of each hinted index to use when running
    // geoNear and examining the output of $indexStats.
    const indexMap = {
        "simpleGeoNearIndex": {location: "2dsphere"},
        "compoundGeoNearIndex": {_id: 1, location: "2dsphere", state: 1}
    };

    const geoNearOptions = {
        query: {_id: {$in: ["1", "2", "3"]}, state: "ACTIVE"},
        spherical: true,
        near: {coordinates: [106.65589, 10.787627], type: "Point"},
        distanceField: "distance",
        key: "location"
    };

    // Helper functions which run either a geoNear command directly or an aggregate which runs a
    // $geoNear stage and verify that it works as expected.
    let runGeoNearAggregate = function(hint) {
        const pipeline = [{$geoNear: geoNearOptions}, {$project: {id: 1, name: 1, distance: 1}}];
        // Run the aggregate with the hint.
        const aggResult = coll.aggregate(pipeline, {hint: hint});
        // Ensure it returns exactly one document.
        assert.eq(1, aggResult.itcount());
    };

    let runGeoNearCommand = function(hint) {
        let geoNearCommand = {geoNear: collName, hint: hint};
        // Attach geoNear options along with index hint and run it.
        Object.assign(geoNearCommand, geoNearOptions);
        const cmdResult = assert.commandWorked(db.runCommand(geoNearCommand));
        // Ensure it returns exactly one document.
        assert(cmdResult.hasOwnProperty("results"));
        assert.eq(1, cmdResult["results"].length, cmdResult);
    };

    // Helper function which executes 'execGeoNearFunc' with the specified 'indexHint' and
    // verifies that the hinted index was used.
    let verifyIndexHint = function(execGeoNearFunc, indexHint, indexName, expectedNumOps) {
        // Run geoNear with the provided hint.
        execGeoNearFunc(indexHint);

        // Run $indexStats and limit the results to the hinted index.
        const indexStats =
            coll.aggregate([{$indexStats: {}}, {$match: {name: indexName}}]).toArray();

        // Verify that the hinted index was used exactly 'expectedNumOps' times. Note that in the
        // case of a sharded cluster, $indexStats will report per-shard stats, so we loop over
        // the array returned by the aggregate to check the stats for each machine.
        for (let indexEntry of indexStats) {
            assert(indexEntry.hasOwnProperty("accesses"), indexStats);
            const indexAccesses = indexEntry["accesses"];
            assert(indexAccesses.hasOwnProperty("ops"), indexStats);
            assert.eq(expectedNumOps, indexAccesses["ops"], indexStats);
        }
    };

    assert.commandWorked(
        coll.insert({_id: "1", state: "ACTIVE", location: [106, 10], name: "TEST"}));
    for (let indexName of Object.keys(indexMap)) {
        assert.commandWorked(coll.createIndex(indexMap[indexName], {name: indexName}));
    }

    // Run both the aggregate and the geoNear command with two different hinted indexes and confirm
    // that the hinted index gets chosen each time. Each index is hinted both by name and by
    // keyPattern to verify that both means of specifying an index hint work correctly.
    //
    // By testing that two separate index hints get used, we confirm that the index in each
    // query plan gets chosen because it was hinted to the aggregate command and not because the
    // query planner chose it over other indexes.
    for (let indexHintName of Object.keys(indexMap)) {
        let expectedNumOps = 1;
        // Verify that hinting the index name works with both geoNear and $geoNear.
        verifyIndexHint(runGeoNearAggregate, indexHintName, indexHintName, expectedNumOps);
        expectedNumOps++;
        verifyIndexHint(runGeoNearCommand, indexHintName, indexHintName, expectedNumOps);
        expectedNumOps++;

        // Verify that hinting the index key pattern works with both geoNear and $geoNear.
        verifyIndexHint(
            runGeoNearAggregate, indexMap[indexHintName], indexHintName, expectedNumOps);
        expectedNumOps++;
        verifyIndexHint(runGeoNearCommand, indexMap[indexHintName], indexHintName, expectedNumOps);
    }
}());
