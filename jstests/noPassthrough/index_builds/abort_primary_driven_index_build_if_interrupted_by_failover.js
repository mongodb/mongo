/**
 * Tests that a primary-driven index build will be aborted during failover, and that a node
 * rejoining the set will abort theirs as well.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

jsTest.log.info("1. Boot a 3-node ReplSet with an arbiter");
const rst = new ReplSetTest({nodes: [{}, {}, {arbiter: true}]});
rst.startSet();
rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
const primaryNodeId = rst.getNodeId(primary);

const dbName = jsTestName();
const collName = "coll";
const indexSpec = {a: 1};
const indexName = "non_resumable_index_build";

// TODO(SERVER-109349): Remove this check when the feature flag is removed.
if (!FeatureFlagUtil.isPresentAndEnabled(primary.getDB(dbName), "PrimaryDrivenIndexBuilds")) {
    jsTest.log.info("Skipping: featureFlagPrimaryDrivenIndexBuilds is disabled");
    rst.stopSet();
    quit();
}

jsTest.log.info("2. Seed data");
assert.commandWorked(primary.getDB(dbName).createCollection(collName));
for (let i = 0; i < 10; i++) {
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert({a: i}));
}
rst.awaitReplication();

const collNss = primary.getDB(dbName).getCollection(collName).getFullName();

jsTest.log.info("3. Pause index build before commit");
const primFp = configureFailPoint(primary, "hangIndexBuildBeforeSignalPrimaryForCommitReadiness");
const secFp = configureFailPoint(secondary, "hangIndexBuildBeforeSignalPrimaryForCommitReadiness");

jsTest.log.info("4. Start index build on primary");
const awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, collNss, indexSpec, {name: indexName}, [
    ErrorCodes.InterruptedDueToReplStateChange,
]);

primFp.wait();

jsTest.log.info("5. Get buildUUID");
let buildUUID = extractUUIDFromObject(
    IndexBuildTest.assertIndexes(primary.getDB(dbName).getCollection(collName), 2, ["_id_"], [indexName], {
        includeBuildUUIDs: true,
    })[indexName].buildUUID,
);
jsTest.log.info(`buildUUID: ${tojson(buildUUID)}`);

jsTest.log.info("6. CommitQuorum is kDisabled (0) while primary-driven");
assert.commandFailedWithCode(
    primary.getDB(dbName).runCommand({setIndexCommitQuorum: collName, indexNames: [indexName], commitQuorum: 1}),
    ErrorCodes.BadValue,
);

jsTest.log.info("7. Simulate primary failure while build is paused");
rst.stop(primary, undefined, {forRestart: true, skipValidation: true});

awaitIndexBuild({checkExitSuccess: false});

jsTest.log.info("7.5. Check Index Build Resume State was not saved to disk");
assert.eq(false, RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput(".*")));

jsTest.log.info("8. Wait for secondary to step up");
rst.waitForState(primary, ReplSetTest.State.DOWN);
rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
let newPrimary = rst.getPrimary();
assert.eq(newPrimary.host, secondary.host);

let newPriDB = newPrimary.getDB(dbName);
let newPriColl = newPriDB.getCollection(collName);

jsTest.log.info("9. Confirm new primary's index build is paused");
secFp.wait();

jsTest.log.info("10. Allow new primary to abort the index build");
secFp.off();

// "Index build: attempting to abort".
checkLog.containsJson(newPrimary, 4656010, {
    buildUUID: function (uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    },
});

IndexBuildTest.waitForIndexBuildToStop(newPriDB);
IndexBuildTest.assertIndexes(newPriColl, 1, ["_id_"]);

jsTest.log.info("11. Clean up and restart original primary");

const restartedPrimary = rst.start(primaryNodeId, {}, true /* restart */);

jsTest.log.info("12. Ensure that original primary does not resume the aborted index build");

rst.awaitReplication();
IndexBuildTest.assertIndexes(restartedPrimary.getDB(dbName).getCollection(collName), 1, ["_id_"]);

jsTest.log.info("13. Attempt to build an identical index successfully");
assert.commandWorked(newPriDB.getCollection(collName).createIndex(indexSpec, {name: indexName}));
IndexBuildTest.assertIndexes(newPriColl, 2, ["_id_", indexName]);

rst.stopSet();
