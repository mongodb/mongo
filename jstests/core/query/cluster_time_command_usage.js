// Tests for using of system variable $$CLUSTER_TIME in different server modes.
//
// @tags: [requires_fcv_82,
// ]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

(function testClusterTimeUsageInCommands() {
    const testDB = db.getSiblingDB(jsTestName());
    const collName = jsTestName();
    const coll = testDB[collName];

    coll.drop();
    assert.commandWorked(coll.insertOne({a: 1}));

    if (FixtureHelpers.isStandalone(testDB)) {
        // Assert that usage of $$CLUSTER_TIME raises an error in queries in standalone mode.
        assert.commandFailedWithCode(
            testDB.runCommand({find: collName, filter: {}, projection: {"a": "$$CLUSTER_TIME"}}),
            10071200,
            "system variable $$CLUSTER_TIME is not available in standalone mode");
    } else {
        // Assert that the $$CLUSTER_TIME is available in queries in replica set or sharded cluster.
        assert.eq(1, coll.find({}, {"a": "$$CLUSTER_TIME"}).itcount());
    }

    // Check behaviour after an insert of Timestamp(0, 0).
    const coll2 = testDB[collName + "2"];
    coll2.drop();
    assert.commandWorked(coll2.insertOne({b: Timestamp(0, 0)}));

    if (FixtureHelpers.isStandalone(testDB)) {
        assert.commandFailedWithCode(
            testDB.runCommand({find: collName, filter: {}, projection: {"a": "$$CLUSTER_TIME"}}),
            10071200,
            "system variable $$CLUSTER_TIME is not available in standalone mode");

        assert.commandFailedWithCode(
            testDB.runCommand(
                {aggregate: collName, pipeline: [{$project: {"a": "$$CLUSTER_TIME"}}], cursor: {}}),
            10071200,
            "system variable $$CLUSTER_TIME is not available in standalone mode");
    } else {
        assert.eq(1, coll.find({}, {"a": "$$CLUSTER_TIME"}).itcount());
    }
})();
