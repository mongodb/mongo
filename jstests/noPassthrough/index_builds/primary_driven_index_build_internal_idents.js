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
const secondary = rst.getSecondary();

// TODO(SERVER-109578): Remove this check when the feature flag is removed.
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

function runTest({unique}) {
    const primaryColl = primaryDB.getCollection(collName);

    assert(primaryColl.drop());
    assert.commandWorked(primaryColl.insert({a: 1}));

    rst.awaitReplication();

    const indexOptions = {unique: unique};
    let awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, primaryColl.getFullName(), {a: 1}, indexOptions);
    awaitIndexBuild();

    const indexIdent = getUriForIndex(primaryColl, "a_1");
    // Index idents take the form "index-<UUID>".
    const uniqueTag = indexIdent.substring(indexIdent.indexOf("-") + 1);
    const sorterIdent = "internal-sorter-" + uniqueTag;
    const sideWritesIdent = "internal-sideWrites-" + uniqueTag;
    const skippedRecordsTrackerIdent = "internal-skippedRecordsTracker-" + uniqueTag;
    const constraintViolationsIdent = "internal-constraintViolations-" + uniqueTag;

    const expectedIndexBuildInfo = {
        "indexIdent": indexIdent,
        "sorterIdent": sorterIdent,
        "sideWritesIdent": sideWritesIdent,
        "skippedRecordsTrackerIdent": skippedRecordsTrackerIdent,
    };
    if (unique) {
        expectedIndexBuildInfo["constraintViolationsTrackerIdent"] = constraintViolationsIdent;
    }

    checkLog.containsRelaxedJson(primary, 20384, {
        "indexBuildInfo": expectedIndexBuildInfo,
    });

    checkLog.containsRelaxedJson(secondary, 20384, {
        "indexBuildInfo": expectedIndexBuildInfo,
    });
}

runTest({unique: false});
runTest({unique: true});

rst.stopSet();
