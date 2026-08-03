/*
 * Tests basic movePrimary behaviour.
 *
 * @tags: [
 *   requires_2_or_more_shards,
 *   does_not_support_stepdowns,
 *   # Expects databases to be in specific places
 *   assumes_stable_shard_list,
 * ]
 */

import {getRandomShardName} from "jstests/libs/cluster_helpers/sharded_cluster_fixture_helpers.js";

const testDB = db.getSiblingDB("test_db");
testDB.dropDatabase();

const coll = testDB["coll"];

const N = 250;

function doInserts(n) {
    jsTestLog("Inserting " + n + " entries.");

    let docs = [];
    for (let i = -(n - 1) / 2; i < n / 2; i++) {
        docs.push({x: i});
    }
    coll.insertMany(docs);
}

assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));

let initPrimaryShard = testDB.getDatabasePrimaryShardId();

doInserts(N);
assert.eq(N, coll.countDocuments({}));

let otherShard = getRandomShardName(db, /* exclude = */ initPrimaryShard);

jsTestLog("Move primary to another shard and check content.");
let res = testDB.adminCommand({movePrimary: testDB.getName(), to: otherShard});
// TODO SERVER-98118: Remove this exclusion since we've banned movePrimary during 9.0 FCV transitions.
if (!TestData.isRunningFCVUpgradeDowngradeSuite) {
    assert.commandWorked(res);
} else {
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
    if (res.code === ErrorCodes.ConflictingOperationInProgress) {
        quit();
    }
}
doInserts(N);
assert.eq(2 * N, coll.countDocuments({}));
assert.eq(otherShard, testDB.getDatabasePrimaryShardId());

jsTestLog("Move primary to the original shard and check content.");
res = testDB.adminCommand({movePrimary: testDB.getName(), to: initPrimaryShard});
// TODO SERVER-98118: Remove this exclusion since we've banned movePrimary during 9.0 FCV transitions.
if (!TestData.isRunningFCVUpgradeDowngradeSuite) {
    assert.commandWorked(res);
} else {
    assert.commandWorkedOrFailedWithCode(res, ErrorCodes.ConflictingOperationInProgress);
    if (res.code === ErrorCodes.ConflictingOperationInProgress) {
        quit();
    }
}
doInserts(N);
assert.eq(3 * N, coll.countDocuments({}));
assert.eq(initPrimaryShard, testDB.getDatabasePrimaryShardId());
