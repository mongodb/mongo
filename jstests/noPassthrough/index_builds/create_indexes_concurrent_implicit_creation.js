/**
 * Tests that createIndexes does not report success without creating the index when two concurrent
 * createIndexes calls race to implicitly create the same new collection. The losing call can
 * observe the winner's collection as already existing while it is still commit-pending, then fail
 * to resolve that UUID. createIndexes used to treat that NamespaceNotFound as "collection dropped"
 * and return success even though the requested index was never built.
 *
 * hangBeforePublishingCatalogUpdates holds the winner after it is marked commit-pending and before
 * it is published. hangCreateIndexesBeforeStartingIndexBuild then parks the loser after it has
 * captured that pending UUID and before it hands off to the IndexBuildsCoordinator, so the UUID
 * lookup runs while the collection is still unpublished.
 *
 * In the field, the loser's own implicit-creation attempt hits a WriteConflictException against
 * the winner first and only takes the "already exists" fast path on writeConflictRetry's retry.
 * Starting the loser after the winner is already commit-pending reaches that same fast path
 * directly, without needing to reproduce the write conflict itself.
 *
 * @tags: [
 *   requires_replication,
 *   # This test coordinates parallel threads and a failpoint-gated hang; execution-control
 *   # prioritization can delay a thread past the synchronization timeout.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {checkLog} from "src/mongo/shell/check_log.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();

const dbName = jsTestName();
const collName = "docs";
const ns = dbName + "." + collName;
const loserComment = "createIndexesConcurrentImplicitCreationLoser";

function runCreateIndexes(host, dbName, collName, key, name, comment) {
    const cmd = {
        createIndexes: collName,
        indexes: [{key: key, name: name}],
    };
    if (comment) {
        cmd.comment = comment;
    }
    return new Mongo(host).getDB(dbName).runCommand(cmd);
}

const hangBeforePublish = configureFailPoint(primary, "hangBeforePublishingCatalogUpdates", {
    collectionNS: ns,
});
const hangBeforeRegister = configureFailPoint(
    primary,
    "hangCreateIndexesBeforeStartingIndexBuild",
    {
        comment: loserComment,
    },
);

const winner = new Thread(runCreateIndexes, primary.host, dbName, collName, {a: 1}, "a_1", "");
const loser = new Thread(
    runCreateIndexes,
    primary.host,
    dbName,
    collName,
    {b: 1},
    "b_1",
    loserComment,
);

let winnerStarted = false;
let loserStarted = false;
try {
    try {
        winner.start();
        winnerStarted = true;
        hangBeforePublish.wait();

        loser.start();
        loserStarted = true;
        hangBeforeRegister.wait();

        hangBeforeRegister.off();

        // Keep the winner unpublished until the loser has attempted to resolve its pending UUID.
        assert.soon(
            () => checkLog.checkContainsOnceJson(primary, 13282200, {namespace: ns}),
            "loser did not encounter NamespaceNotFound while resolving the pending UUID",
        );
    } finally {
        hangBeforeRegister.off();
        hangBeforePublish.off();
        if (winnerStarted) {
            winner.join();
        }
        if (loserStarted) {
            loser.join();
        }
    }

    const winnerRes = winner.returnData();
    const loserRes = loser.returnData();
    assert.commandWorked(winnerRes);
    assert.commandWorked(loserRes);
    assert.eq(
        true,
        winnerRes.createdCollectionAutomatically,
        "winner did not implicitly create the collection",
        {winnerRes},
    );
    assert.eq(
        false,
        loserRes.createdCollectionAutomatically,
        "loser did not observe the commit-pending collection as already existing",
        {loserRes},
    );

    IndexBuildTest.assertIndexes(primary.getDB(dbName).getCollection(collName), 3, [
        "_id_",
        "a_1",
        "b_1",
    ]);
} finally {
    rst.stopSet();
}
