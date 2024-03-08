/**
 * Test the behavior of a dropDatabase command during an aggregation containing $out.
 *
 * @tags: [
 *   assumes_unsharded_collection,
 *   do_not_wrap_aggregations_in_facets,
 *   assumes_read_concern_unchanged,
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

function runTest(st, testDb, portNum) {
    // TODO SERVER-80853 add 'hangWhileBuildingDocumentSourceOutBatch' to this list
    for (const failpointName
             of ["outWaitAfterTempCollectionCreation", "outWaitBeforeTempCollectionRename"]) {
        const coll = testDb.out_source_coll;
        coll.drop();

        const targetColl = testDb.out_target_coll;
        targetColl.drop();

        assert.commandWorked(coll.insert({val: 0}));
        assert.commandWorked(coll.createIndex({val: 1}));

        let res = FixtureHelpers.runCommandOnEachPrimary({
            db: testDb.getSiblingDB("admin"),
            cmdObj: {
                configureFailPoint: failpointName,
                mode: "alwaysOn",
            }
        });
        res.forEach(cmdResult => assert.commandWorked(cmdResult));

        const aggDone = startParallelShell(() => {
            const targetDB = db.getSiblingDB("out_drop_temp");
            // There are a number of possible error codes depending on configuration and index build
            // options.
            assert.commandFailed(targetDB.runCommand(
                {aggregate: "out_source_coll", pipeline: [{$out: "out_target_coll"}], cursor: {}}));
            const collList = assert.commandWorked(targetDB.runCommand({listCollections: 1}));
            assert.eq(collList.cursor.firstBatch.length, 0);
        }, portNum);

        waitForCurOpByFailPointNoNS(testDb, failpointName);

        assert.commandWorked(testDb.runCommand({dropDatabase: 1}));

        FixtureHelpers.runCommandOnEachPrimary({
            db: testDb.getSiblingDB("admin"),
            cmdObj: {
                configureFailPoint: failpointName,
                mode: "off",
            }
        });
        aggDone();
    }
}

const conn = MongoRunner.runMongod({});
runTest(null, conn.getDB("out_drop_temp"), conn.port);
MongoRunner.stopMongod(conn);
const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
// TODO SERVER-87422 enable this test with this feature flag.
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
if (FeatureFlagUtil.isEnabled(st.s, "TrackUnshardedCollectionsOnShardingCatalog")) {
    st.stop()
    quit();
}
runTest(st, st.s.getDB("out_drop_temp"), st.s.port);
st.stop();
