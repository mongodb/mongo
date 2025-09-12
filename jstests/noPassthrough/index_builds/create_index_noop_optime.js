/**
 * This test is a repro case for SERVER-107485. When a createIndex was discovered to be a no-op when
 * starting the index build (and not as part of the pre-start checks), it would capture the current
 * latest optime and use that as the optime sent in the reply, which would be *before*
 * the commitIndexBuild optime if the pre-existing index hadn't been committed yet.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";
import {Thread} from "jstests/libs/parallelTester.js";

const replSet = new ReplSetTest({nodes: 1});

replSet.startSet();
replSet.initiate();

const dbName = "test";
const collName = jsTestName();

const primary = replSet.getPrimary();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

// Collection needs to be non-empty for an index build to occur
assert.commandWorked(coll.insert({a: 1}));

// In a parallel shell, start an index build and block it after it's done the initial check for if
// the indexes already exist, but before it starts actually building the indexes. We need to use a
// comment here to only block this index build and not the next one we'll be creating.
const comment = "toBlockBeforeRegister";
const hangCreateIndexesBeforeStartingIndexBuildFP = configureFailPoint(
    db,
    "hangCreateIndexesBeforeStartingIndexBuild",
    {comment},
);
function createIndexThread(host, dbName, collName, comment) {
    return assert.commandWorked(
        new Mongo(host).getDB(dbName).runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "a_1"}],
            comment: comment,
        }),
    );
}
const thread = new Thread(createIndexThread, db.getMongo().host, dbName, collName, comment);
thread.start();
hangCreateIndexesBeforeStartingIndexBuildFP.wait();

// Build the same index, but this time block it before commit instead of before start
jsTestLog("Starting an index build on {a: 1} and hanging");
let hangBeforeCommitFP = configureFailPoint(db, "hangIndexBuildBeforeCommit");
let awaitIndexBuild = IndexBuildTest.startIndexBuild(primary, coll.getFullName(), {a: 1});
hangBeforeCommitFP.wait();

// Unblock the first index build and wait for it to fail to register. The original bug here was that
// this step would set the optime for the response (which is before the commit optime, as that
// hasn't happened yet).
hangCreateIndexesBeforeStartingIndexBuildFP.off();
checkLog.containsJson(db, 20450);

// Complete the paused index build. It committing will wake up the failed index build which was
// waiting for it, and both will complete.
hangBeforeCommitFP.off();
awaitIndexBuild();
thread.join();

// Get the commitIndexBuild oplog entry, and compare its optime to the optime returned by the no-op
// index build. In this case they should be equal, but it's also okay for the reply time to be
// greater than the actual commit time.
const createIndexResult = thread.returnData();
const oplog = primary.getDB("local").getCollection("oplog.rs");
const commitEntry = oplog.find({"o.commitIndexBuild": "create_index_noop_optime"}).toArray()[0];

assert.gte(createIndexResult.operationTime, commitEntry.ts);

replSet.stopSet();
