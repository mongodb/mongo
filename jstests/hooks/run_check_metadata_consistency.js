import {MetadataConsistencyChecker} from "jstests/libs/check_metadata_consistency_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

assert.neq(typeof db, 'undefined', 'No `db` object, is the shell connected to a server?');

{
    // Check that we are running on a sharded cluster
    let isShardedCluster = false;
    try {
        isShardedCluster = FixtureHelpers.isMongos(db);
    } catch (e) {
        if (ErrorCodes.isRetriableError(e.code) || ErrorCodes.isInterruption(e.code) ||
            ErrorCodes.isNetworkTimeoutError(e.code)) {
            jsTest.log(
                `Aborted metadata consistency check due to retriable error during topology discovery: ${
                    e}`);
            quit();
        } else {
            throw e;
        }
    }
    assert(isShardedCluster, "Metadata consistency check must be run against a sharded cluster");
}

const mongos = db.getMongo();
MetadataConsistencyChecker.run(mongos);
