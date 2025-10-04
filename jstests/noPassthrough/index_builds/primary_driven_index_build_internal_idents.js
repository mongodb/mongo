/**
 * Tests that primary driven index builds deterministically generates internal idents and
 * primaries and secondaries use the same idents.
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {getUriForIndex} from "jstests/disk/libs/wt_file_helper.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "coll";
const primary = rst.getPrimary();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
const secondary = rst.getSecondary();

// TODO(SERVER-109349): Remove this check when the feature flag is removed.
// TODO(SERVER-105853): Remove this check when the feature flag is removed.
if (
    !FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds") ||
    !FeatureFlagUtil.isPresentAndEnabled(primaryDB, "ReplicateLocalCatalogIdentifiers")
) {
    jsTestLog(
        "Skipping primary_driven_index_build_internal_idents.js because featureFlagPrimaryDrivenIndexBuilds" +
            " and/or featureFlagReplicateLocalCatalogIdentifiers is disabled",
    );
    rst.stopSet();
    quit();
}

assert.commandWorked(primaryColl.insert({a: 1}));

rst.awaitReplication();

let awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1});
awaitIndexBuild();

const indexIdent = getUriForIndex(primaryColl, "a_1");
const sorterIdent = "internal-sorter-" + indexIdent;
const sideWritesIdent = "internal-sideWrites-" + indexIdent;
const skippedRecordsTrackerIdent = "internal-skippedRecordsTracker-" + indexIdent;
const constraintViolationsIdent = "internal-constraintViolations-" + indexIdent;

checkLog.containsRelaxedJson(primary, 20384, {
    "indexBuildInfo": {
        "indexIdent": indexIdent,
        "sorterIdent": sorterIdent,
        "sideWritesIdent": sideWritesIdent,
        "skippedRecordsTrackerIdent": skippedRecordsTrackerIdent,
        "constraintViolationsTrackerIdent": constraintViolationsIdent,
    },
});

checkLog.containsRelaxedJson(secondary, 20384, {
    "indexBuildInfo": {
        "indexIdent": indexIdent,
        "sorterIdent": sorterIdent,
        "sideWritesIdent": sideWritesIdent,
        "skippedRecordsTrackerIdent": skippedRecordsTrackerIdent,
        "constraintViolationsTrackerIdent": constraintViolationsIdent,
    },
});

rst.stopSet();
