/**
 * Tests that the deprecated `md.timeseriesBucketingParametersHaveChanged` catalog field is removed
 * on FCV upgrade.
 *
 * @tags: [
 *   requires_timeseries,
 *   featureFlagRemoveLegacyTimeseriesBucketingParametersHaveChanged,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = jsTestName();
const collName = jsTestName();

// Launch a v8.0 replica set with featureFlagTSBucketingParametersUnchanged=true to set the
// `md.timeseriesBucketingParametersHaveChanged` catalog field, as the latest versions don't set it.
if (lastLTSFCV != "8.0") {
    print("Skipping test because last LTS FCV is no longer 8.0");
    quit();
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet(
    {binVersion: 'last-lts', setParameter: {featureFlagTSBucketingParametersUnchanged: true}});
rst.initiate();

assert.commandWorked(
    rst.getPrimary().getDB(dbName).createCollection(collName, {timeseries: {timeField: 't'}}));

// Upgrade the replica set to the latest FCV and verify that the flag is removed
rst.stopSet(null /* signal */, true /* forRestart */);
rst.startSet({binVersion: 'latest'}, true /* restart */);

const db = rst.getPrimary().getDB(dbName);

const getCatalogEntry = () =>
    db.getCollection(`system.buckets.${collName}`).aggregate([{$listCatalog: {}}]).toArray()[0];

assert.eq(false, getCatalogEntry().md.timeseriesBucketingParametersHaveChanged);
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert.eq(undefined, getCatalogEntry().md.timeseriesBucketingParametersHaveChanged);

rst.stopSet();
