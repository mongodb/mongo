
/**
 * Test that reset placement history loads the correct snapshot. This is actually a test for
 * ensuring that an aggregation run on a local shard with snapshot read concern will only see the
 * snapshot data
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

(function() {
'use strict';

var st = new ShardingTest({shards: 2});
var mongos = st.s;

var dbName = jsTestName();
var kNssColl1 = dbName + ".coll1";
var kNssColl2 = dbName + ".coll2";
var kNssColl3 = dbName + ".coll3";

/*The test will attempt to simulate a fresh initialization of the placement history (empty
 * placement history and a non-empty catalog.collections/databases). While this is ok for the
 * purpose of the test, it will be caught as a routing table inconsistency and the check should be
 * disabled*/
TestData.skipCheckRoutingTableConsistency = true;

jsTest.log("Running reset placement history should use snapshot read concern");
{
    jsTest.log("configuring failpoint");
    const failPoint =
        configureFailPoint(st.configRS.getPrimary(),
                           'initializePlacementHistoryHangAfterSettingSnapshotReadConcern',
                           {mode: 'alwaysOn'});

    // shard 2 collections - part of the snapshot
    assert.commandWorked(st.s.adminCommand({shardCollection: kNssColl1, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: kNssColl2, key: {_id: 1}}));

    // Starts parallel shell to run the command that will hang.
    let awaitShell = startParallelShell(function() {
        jsTest.log("PARALLEL SHELL: Running reset placement history");
        assert.commandWorked(db.getSiblingDB("admin").runCommand({resetPlacementHistory: 1}));
    }, st.s.port);

    jsTest.log("Waiting for reset placement history to hang");
    failPoint.wait();

    // shard collection - not part of the snapshot
    assert.commandWorked(st.s.adminCommand({shardCollection: kNssColl3, key: {_id: 1}}));

    // cleanup the placement history (so that it will only contain the documents inserted by the
    // reset command once the failpont is turned off).
    st.config.placementHistory.deleteMany({});

    // unhang the reset placement history
    jsTest.log("Unhanging reset placement history and will wait for it to finish");
    failPoint.off();
    awaitShell();

    // reset placement history will register all the data found in the snapshot read of
    // config.collection. The insertion of the third collection happened
    // after we read the snapshot so it should not be registered (note: this is only because we
    // forced a cleanup of the placement history, otherwise the third collection would have been
    // registered as part of the insertion)
    let entryColl1 = st.config.placementHistory.findOne({nss: kNssColl1});
    let entryColl2 = st.config.placementHistory.findOne({nss: kNssColl2});
    let entryColl3 = st.config.placementHistory.findOne({nss: kNssColl3});
    assert.neq(entryColl1, null);
    assert.neq(entryColl2, null);
    timestampCmp(entryColl1.timestamp, entryColl2.timestamp);
    assert.eq(entryColl3, null);

    st.stop();
}
})();
