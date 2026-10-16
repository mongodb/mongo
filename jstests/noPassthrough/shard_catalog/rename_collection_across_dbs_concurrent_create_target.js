/**
 * Regression test for SERVER-135508. Ensure rename across DB without dropTarget: true fails as
 * expected if the target DB and collection is created after the rename acquired database locks.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const sourceDb = primary.getDB("sourceDb");
const targetDb = primary.getDB("targetDb");

const sourceColl = sourceDb.getCollection("sourceColl");
const targetColl = targetDb.getCollection("targetColl");
assert.commandWorked(sourceColl.insertOne({_id: "source"}));

// Hang rename after locking the DBs, but before locking the collections.
const fp = configureFailPoint(primary, "hangRenameCollectionAcrossDatabasesAfterAcquiringDbLocks");
const renameThread = new Thread(
    (host, from, to) => new Mongo(host).adminCommand({renameCollection: from, to}),
    primary.host,
    sourceColl.getFullName(),
    targetColl.getFullName(),
);
renameThread.start();
fp.wait();

// Create the target collection, which will implicitly also create the target DB.
// This will succeed, since rename hasn't taken strong locks yet.
assert.commandWorked(targetColl.insertOne({_id: "target"}));

// Let rename continue. It should fail before doing any changes.
fp.off();

renameThread.join();
const renameResult = renameThread.returnData();
assert.commandFailedWithCode(renameResult, ErrorCodes.NamespaceExists);
assert.sameMembers([{_id: "source"}], sourceColl.find().toArray());
assert.sameMembers([{_id: "target"}], targetColl.find().toArray());

rst.stopSet();
