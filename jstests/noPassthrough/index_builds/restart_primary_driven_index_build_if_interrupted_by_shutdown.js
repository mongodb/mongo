/**
 * Tests that a primary-driven index build gets restarted even after a clean shutdown.
 *
 * @tags: [
 *   requires_persistence,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

jsTestLog("1. Boot a ReplSet with 1 node");
let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let dbName = jsTestName();
let collName = "coll";
let indexSpec = {a: 1};
let indexName = "non_resumable_index_build";
let primary = rst.getPrimary();
let primaryDB = primary.getDB(dbName);
let coll = primaryDB.getCollection(collName);
let collNss = coll.getFullName();

// TODO(SERVER-109349): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primaryDB, "PrimaryDrivenIndexBuilds")) {
    jsTestLog(
        "Skipping restart_primary_driven_index_build_if_interrupted.js because featureFlagPrimaryDrivenIndexBuilds is disabled",
    );
    rst.stopSet();
    quit();
}

jsTestLog("2. Insert data");
for (let i = 0; i < 10; i++) {
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert({a: i}));
}
rst.awaitReplication();

jsTestLog("3. Pause Index Build after intialization");
let primFp = configureFailPoint(primary, "hangAfterInitializingIndexBuild");

jsTestLog("4. Begin Index Build on all nodes");
const awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, collNss, indexSpec, {name: indexName}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

primFp.wait();

jsTestLog("5. Obtain buildUUID");
let buildUUID = extractUUIDFromObject(
    IndexBuildTest.assertIndexes(coll, 2, ["_id_"], [indexName], {includeBuildUUIDs: true})[indexName].buildUUID,
);
jsTestLog("5. Obtained buildUUID ", buildUUID);

jsTestLog("6. Restarting the primary");
rst.stop(primary, undefined, {forRestart: true, skipValidation: true});

awaitIndexBuild({checkExitSuccess: false});

jsTestLog("7. Check Index Build Resume State was not saved to disk");
assert(RegExp("20347.*" + buildUUID).test(rawMongoProgramOutput(".*")));
assert.eq(false, RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput(".*")));
rst.start(primary, {}, true);

jsTestLog("8. Wait for primary to get re-elected.");
primary = rst.getPrimary();
primaryDB = primary.getDB(dbName);

jsTestLog("9. Finish Index Build on Primary");
primFp.off();

jsTestLog("10. Check that primary restarted the index build");
checkLog.containsJson(primary, 20660, {
    buildUUID: function (uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    },
    method: "Primary driven",
});

jsTestLog("11. Check that the index build completed successfully on primary");
checkLog.containsJson(primary, 20663, {
    buildUUID: function (uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    },
    namespace: coll.getFullName(),
});

rst.stopSet();
