/**
 * Test that renaming a viewless timeseries collection across databases during FCV downgrade fails,
 * because it may cause it to be missed by the viewless-to-viewful conversion (see SERVER-123066).
 * TODO(SERVER-123292): Remove once 9.0 becomes last LTS.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

if (lastLTSFCV != "8.0") {
    jsTest.log.info("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

function runTest(db, failpointConn) {
    const src = db.ts;
    const dst = db.getSiblingDB("other").ts;
    assert.commandWorked(db.createCollection(src.getName(), {timeseries: {timeField: "time"}}));

    // Fail setFCV while downgrading to viewful timeseries.
    const fp = configureFailPoint(failpointConn, "failDowngrading");
    assert.commandFailed(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    // Cross-DB rename must fail while downgrading.
    assert.commandFailedWithCode(
        db.adminCommand({renameCollection: src.getFullName(), to: dst.getFullName()}),
        ErrorCodes.IllegalOperation,
    );

    fp.off();
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    // If viewless timeseries is re-enabled, cross-DB rename works.
    assert.commandWorked(db.adminCommand({renameCollection: src.getFullName(), to: dst.getFullName()}));
}

// Replica set.
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    runTest(rst.getPrimary().getDB("test"), rst.getPrimary());
    rst.stopSet();
}

// Sharded cluster.
{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});
    runTest(st.s.getDB("test"), st.rs0.getPrimary());
    st.stop();
}
