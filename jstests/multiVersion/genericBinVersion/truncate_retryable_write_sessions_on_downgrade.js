/**
 * Test that retryable write sessions are truncated so they will return IncompleteTransactionHistory
 * after FCV downgrade.  This behavior and test can be removed when SERVER-87563 is in all versions
 * which could be downgraded to.
 *
 * TODO(SERVER-84271): Remove this test when featureFlagReplicateVectoredInsertsTransactionally is
 * removed.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// Start up a replica set with the latest binary version and the
// ReplicateVectoredInsertsTransactionally feature flag.  This feature flag results in oplog
// entries which can't be read by older versions

const nodeOption = {
    binVersion: 'latest',
    setParameter: {"featureFlagReplicateVectoredInsertsTransactionally": true}
};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary-eligible node available.
const replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate();

let primary = replSet.getPrimary();
let testDB = primary.getDB('test');
const admin = primary.getDB('admin');
const collName = TestData.testName;

// Confirm feature flag is enabled.
assert(FeatureFlagUtil.isEnabled(admin, "ReplicateVectoredInsertsTransactionally"));

// Create some retryable writes with multiOpType:1, which can't be parsed by old versions.
const lsid0 = {
    "id": UUID()
};
const lsid1 = {
    "id": UUID()
};

const docs0 = [{_id: 0, x: 0}, {_id: 1, x: 1}];
const docs1 = [{_id: 10, x: 10}, {_id: 11, x: 11}];
const docs2 = [{_id: 20, x: 20}, {_id: 21, x: 21}];
const docs3 = [{_id: 30, x: 30}, {_id: 31, x: 31}];
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs0, lsid: lsid0, txnNumber: NumberLong(10)}));
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs1, lsid: lsid1, txnNumber: NumberLong(20)}));
// Retry should work:
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs0, lsid: lsid0, txnNumber: NumberLong(10)}));
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs1, lsid: lsid1, txnNumber: NumberLong(20)}));

assert.eq(
    2,
    primary.getDB("local")
        .oplog.rs
        .find({$and: [{multiOpType: 1}, {"$or": [{"lsid.id": lsid0.id}, {"lsid.id": lsid1.id}]}]})
        .itcount());

// Downgrade FCV and confirm the feature flag is now disabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert(!FeatureFlagUtil.isEnabled(admin, "ReplicateVectoredInsertsTransactionally"));

// Downgrade the set to the last-lts version.
delete replSet.nodeOptions['n0'].setParameter.featureFlagReplicateVectoredInsertsTransactionally;
delete replSet.nodeOptions['n1'].setParameter.featureFlagReplicateVectoredInsertsTransactionally;
replSet.upgradeSet({binVersion: 'last-lts'})

primary = replSet.getPrimary();
testDB = primary.getDB('test');
// Retry should fail with incomplete transaction history.
assert.commandFailedWithCode(
    testDB.runCommand({insert: collName, documents: docs0, lsid: lsid0, txnNumber: NumberLong(10)}),
    ErrorCodes.IncompleteTransactionHistory);
assert.commandFailedWithCode(
    testDB.runCommand({insert: collName, documents: docs1, lsid: lsid1, txnNumber: NumberLong(20)}),
    ErrorCodes.IncompleteTransactionHistory);

// The sessions should still be usable with later transaction numbers.
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs2, lsid: lsid0, txnNumber: NumberLong(11)}));
assert.commandWorked(testDB.runCommand(
    {insert: collName, documents: docs3, lsid: lsid1, txnNumber: NumberLong(21)}));

replSet.stopSet();
