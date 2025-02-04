/*
 * Validate that replica sets do not support the $shardedDataDistribution aggregation stage.
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

if (FixtureHelpers.isMongos(db) || TestData.testingReplicaSetEndpoint) {
    // The functionalities of $shardedDataDistribution against router ports are covered in other
    // test files.
    quit();
}

assert.commandFailedWithCode(
    db.adminCommand({aggregate: 1, pipeline: [{$shardedDataDistribution: {}}], cursor: {}}),
    6789101);
