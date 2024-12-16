/**
 * Test the behavior of a dropDatabase command during an aggregation containing $out.
 */

import {waitForCurOpByFailPointNoNS} from "jstests/libs/curop_helpers.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
            const res = targetDB.runCommand(
                {aggregate: "out_source_coll", pipeline: [{$out: "out_target_coll"}], cursor: {}});

            // When the dropDatabase and $out happen concurrently, the result must be the same as if
            // they happened serially: $out then drop (result is non-existant collection) or, drop
            // then $out (result is empty collection). On replSets we get the former and sharded
            // clusters we get the latter, but both behaviors are acceptable.
            if (!res.ok) {
                // There are a number of possible error codes depending on configuration and index
                // build options.
                const collList = assert.commandWorked(targetDB.runCommand({listCollections: 1}));
                assert.eq(collList.cursor.firstBatch.length, 0);
            } else {
                assert.eq(targetDB["out_target_coll"].countDocuments({}), 0);
            }
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
runTest(st, st.s.getDB("out_drop_temp"), st.s.port);
st.stop();
