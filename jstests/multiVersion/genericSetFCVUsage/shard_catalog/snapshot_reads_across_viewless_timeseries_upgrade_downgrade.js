/**
 * Tests snapshot reads across the viewful and viewless timeseries upgrading and downgrade.
 * TODO(SERVER-114573): Remove this test once 9.0 becomes lastLTS.
 *
 * @tags: [
 *   requires_timeseries,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

if (lastLTSFCV != "8.0") {
    print("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());

function findWithSnapshotAcrossTimeseriesUpgradeDowngrade(coll, initialFCV, fcvTransitions) {
    const expectedDocs = [{t: ISODate("2019-01-30T07:30:12.596Z"), temp: 123}];

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: initialFCV, confirm: true}));
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: "t"}}));
    assert.commandWorked(coll.insertMany(expectedDocs));

    const clusterTimeBeforeUpgradeDowngrade = db.getMongo().getClusterTime().clusterTime;
    for (const fcv of fcvTransitions) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));
    }

    const r = coll.find({}, {_id: 0}).readConcern("snapshot", clusterTimeBeforeUpgradeDowngrade).toArray();
    assert.sameMembers(expectedDocs, r);
}

// Reads across the upgrade succeed.
findWithSnapshotAcrossTimeseriesUpgradeDowngrade(db.viewful_to_viewless, lastLTSFCV, [latestFCV]);

// Reads across the downgrade succeed.
findWithSnapshotAcrossTimeseriesUpgradeDowngrade(db.viewless_to_viewful, latestFCV, [lastLTSFCV]);

// Reads across upgrade+downgrade succeed.
findWithSnapshotAcrossTimeseriesUpgradeDowngrade(db.viewful_to_viewless_and_back, lastLTSFCV, [latestFCV, lastLTSFCV]);

// Reads across downgrade+upgrade succeed.
findWithSnapshotAcrossTimeseriesUpgradeDowngrade(db.viewless_to_viewful_and_back, latestFCV, [lastLTSFCV, latestFCV]);

rst.stopSet();
