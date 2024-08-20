/**
 * Verifies that if we do an upgrade during a vectored insert that fails during batch application
 * that it will not succeed.  A regression test for BF-32345.
 */

import "jstests/multiVersion/libs/multi_rs.js";

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

"use strict";

const latestVersion = "latest";
const rst = new ReplSetTest(
    {name: jsTestName(), nodes: [{binVersion: latestVersion}, {binVersion: latestVersion}]});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const testDB = primary.getDB("test");
const testColl = testDB.testColl;
// Start in downgrade FCV,
jsTestLog("Downgrading FCV");
assert.commandWorked(
    testDB.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

// Insert a document to cause an error in batch insertion
assert.commandWorked(testColl.insert([{"_id": "5"}]));

// Switch to upgrade FCV first.  We have to stop this in the middle so we can get the collmods in
// _upgradeServerMetadata to finish before allowing FCV to actually change.  This is a very
// unlikely race!
let upgradeFp = configureFailPoint(primary, "hangWhileUpgrading");
jsTestLog("Switching to upgrade FCV");
let upgradeFCVThread = new Thread(function(host) {
    let conn = new Mongo(host);
    assert.commandWorked(
        conn.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
}, primary.host);
upgradeFCVThread.start();
upgradeFp.wait();

// We'll pause the insert while we're inserting documents one-by-one, after inserting "2".
let insertFp = configureFailPoint(
    primary, "hangAfterCollectionInserts", {last_id: "2", collectionNS: testColl.getFullName()});

let insertThread = new Thread(function(host, testCollFullName) {
    // Disabling the session here means fewer things to block FCV.
    TestData.disableImplicitSessions = true;
    assert(jsTest.options().disableImplicitSessions);
    let conn = new Mongo(host);
    let testColl = conn.getCollection(testCollFullName);
    // Run a batch that will fail on the last document due to a DuplicateKeyError.
    jsTestLog("Inserting data");
    let res = testColl.insert(
        [{"_id": "1"}, {"_id": "2"}, {"_id": "3"}, {"_id": "4"}, {"_id": "5", new: 1}]);
    assert.eq(4, res.nInserted);
    printjson(res);
}, primary.host, testColl.getFullName());
insertThread.start();
insertFp.wait();
// Now we're in an intermediate FCV with an insert running having already checked its FCV in
// insertDocumentsAtomically.  We'll let the upgrade continue now.
jsTestLog("Allowing upgrade to finish");
assert.commandWorked(primary.adminCommand({clearLog: 'global'}));
upgradeFp.off();
// The FCV command will get stuck on another collMod, so we only wait here for the FCV to be set,
// not for the command to finish.
checkLog.containsJson(primary, 5853300, {featureCompatibilityVersion: latestFCV});

// Now let the insert continue in the new FCV.
jsTestLog("Allowing insert to finish");
insertFp.off();
insertThread.join();
upgradeFCVThread.join();

assert.docEq(testColl.find({}).sort({_id: 1}).toArray(),
             [{"_id": "1"}, {"_id": "2"}, {"_id": "3"}, {"_id": "4"}, {"_id": "5"}]);
rst.stopSet();
