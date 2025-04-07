/**
 * Test removal of garbage in config.dropPendingDBs during FCV upgrade.
 * TODO (SERVER-94362): Delete this test and the related failpoints from the source code once 9.0
 * becomes last LTS.
 *
 * @tags: [
 *      featureFlagCreateDatabaseDDLCoordinator,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 1});
const configDB = st.configRS.getPrimary().getDB("config");

assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));

const fpDrop =
    configureFailPoint(st.rs0.getPrimary(), "dropDatabaseCoordinatorPauseAfterConfigCommit");
const fpSetFCV =
    configureFailPoint(st.configRS.getPrimary(), "setFCVPauseAfterReadingConfigDropPedingDBs");

assert.commandWorked(
    st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Insert some garbage into config.dropPendingDBs, including one dangling entry for a database we
// are going to try to drop.
const now = new Date();
const hoursAgo24 = new Date(now - 24 * 60 * 60 * 1000);
const unixTimestamp24HoursAgo = Math.floor(hoursAgo24.getTime() / 1000);

assert.commandWorked(configDB.dropPendingDBs.insert(
    {_id: "test", version: {timestamp: Timestamp(unixTimestamp24HoursAgo, 1)}}));
assert.commandWorked(configDB.dropPendingDBs.insert(
    {_id: "garbage0", version: {timestamp: Timestamp(unixTimestamp24HoursAgo + 1, 1)}}));
assert.commandWorked(configDB.dropPendingDBs.insert(
    {_id: "garbage1", version: {timestamp: Timestamp(unixTimestamp24HoursAgo + 2, 1)}}));
assert.commandWorked(configDB.dropPendingDBs.insert(
    {_id: "garbage2", version: {timestamp: Timestamp(unixTimestamp24HoursAgo + 3, 1)}}));

const joinDrop = startParallelShell(() => {
    assert.commandWorked(db.getSiblingDB("test").runCommand({dropDatabase: 1}));
}, st.s.port);

fpDrop.wait();

const joinSetFCV = startParallelShell(() => {
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}, st.s.port);

fpSetFCV.wait();

fpSetFCV.off();
fpDrop.off();

joinDrop();
joinSetFCV();

// The garbage should be gone, and dropDatabase worked.
assert.eq(configDB.dropPendingDBs.find().toArray().length, 0);
assert.eq(configDB.databases.find().toArray().length, 0);

// createDatabase still works
assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));

st.stop();
