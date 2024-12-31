/**
 * Tests an index build is resilient in the face of an unclean shutdown on an
 * already-resumed resumable index build. If the resumed index build fails to run to completion
 * before the unclean shutdown, it will restart from the beginning on the next server startup.
 *
 * As seen in the field, it is possible that secondaries participating in a replicated index build
 * take much longer than the primary. A few examples are asymmetric load, underprovisioning of the
 * node, an OOM, or other interruption that reboots the secondary more than once causing it to
 * restart the index build from the beginning. This test is checking that the secondary experiencing
 * a range of shutdowns after the primary has been ready to commit, eventually leads to a successful
 * commit and completion of the replicated index build.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_build.js";

jsTestLog("1. Boot ReplSet of 2 nodes");
let rst = new ReplSetTest({
    nodes: [
        {},                         // primary
        {rsConfig: {priority: 0}},  // disallow elections on secondary
    ]
});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();

let dbName = jsTestName();
let collName = "coll";
let indexSpec = {a: 1};
let indexName = "resumable_index_build_secondary_unclean1";
let coll = primary.getDB(dbName).getCollection(collName);

jsTestLog("2. Insert data");
for (let i = 0; i < 10; i++) {
    assert.commandWorked(primary.getDB(dbName).getCollection(collName).insert({a: i}));
}
rst.awaitReplication();

jsTestLog("3. Pause Index Build on all nodes after intialization");
let secFp = configureFailPoint(secondary, 'hangAfterInitializingIndexBuild');
let primFp = configureFailPoint(primary, 'hangAfterInitializingIndexBuild');

jsTestLog("4. Begin Index Build on all nodes");
let createIndexFn = function(dbName, collName, indexSpec, indexName) {
    jsTestLog("Resumable replicated index build in parallel shell");
    jsTestLog("nss: " + dbName + "." + collName + ", indexName: " + indexName +
              " indexSpec: " + tojson(indexSpec));
    assert.commandWorked(
        db.getSiblingDB(dbName).getCollection(collName).createIndex(indexSpec, {name: indexName}));
    jsTestLog("Resumable replicated index build in parallel shell - done");
};

let awaitCreateIndexParallel = startParallelShell(
    funWithArgs(createIndexFn, dbName, collName, indexSpec, indexName), primary.port);
primFp.wait();

jsTestLog("5. Obtain buildUUID");
let buildUUID = extractUUIDFromObject(
    IndexBuildTest
        .assertIndexes(coll, 2, ['_id_'], [indexName], {includeBuildUUIDs: true})[indexName]
        .buildUUID);

jsTestLog("6. Finish Index Build on Primary, voted for commit quorum");
primFp.off();
checkLog.containsJson(primary, 7568000, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    }
});

jsTestLog("7. Reboot Secondary cleanly");
rst.stop(secondary);

jsTestLog("8. Check Index Build Resume State was saved to disk");
assert(RegExp("4841502.*" + buildUUID).test(rawMongoProgramOutput(".*")));
rst.start(secondary, {noCleanData: true});

jsTestLog("9. Check that Secondary resumed index build");
checkLog.containsJson(secondary, 4841700, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    }
});

jsTestLog("10. Unclean shutdown of secondary");
rst.stop(
    secondary, 9, {allowedExitCode: MongoRunner.EXIT_SIGKILL}, {forRestart: true, waitpid: true});

jsTestLog("11. Boot secondary");
rst.start(secondary, undefined, /*restart=*/ true);

jsTestLog("12. Check secondary restarts index build");
checkLog.containsJson(secondary, 20660, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    }
});

jsTestLog("13. Check that Secondary votes for commit quorum");
checkLog.containsJson(secondary, 7568000, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    }
});

jsTestLog("14. Check that the index build completed successfully on primary");
checkLog.containsJson(primary, 20663, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    },
    namespace: coll.getFullName()
});

jsTestLog("15. Check that the index build completed successfully on secondary");
checkLog.containsJson(secondary, 20663, {
    buildUUID: function(uuid) {
        return uuid && uuid["uuid"]["$uuid"] === buildUUID;
    },
    namespace: coll.getFullName()
});

jsTestLog("16. Join with parallel shell that ran the index build");
awaitCreateIndexParallel();

rst.stopSet();
