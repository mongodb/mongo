/**
 * Tests prepared transactions with an explicitly set apiVersion.
 */
// TODO (SERVER-111718): Remove this test if it will be covered elsewhere, or relocate it.
import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Override all commands to explicitly set 'apiVersion'.
import("jstests/libs/override_methods/set_api_version.js");

const dbName = "prepare_txn_with_api";
const collName = "test";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();
let secondary = rst.getSecondary();
const testDB = primary.getDB(dbName);
const testColl = testDB.getCollection(collName);

let oldDoc = {_id: 42};
let inc = 1;
// First create the collection.
assert.commandWorked(testColl.insert(oldDoc));

const runTest = function (failover, commit) {
    jsTestLog(`Testing ${commit ? "Commiting" : "Aborting"} with ${failover ? "Failover" : "Restarting Primary"}.`);
    const newDoc = {_id: 42, x: inc++};
    let session = primary.startSession();
    const sessionColl = session.getDatabase(dbName).getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionColl.update(oldDoc, newDoc));

    // Prepare a transaction. This will be replicated to the secondary.
    const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
    rst.awaitReplication();

    // Stepping up Secondary.
    rst.stepUp(secondary);
    rst.awaitReplication();
    rst.awaitSecondaryNodes(null, [primary]);
    rst.waitForState(secondary, ReplSetTest.State.PRIMARY);
    if (failover) {
        session = new _DelegatingDriverSession(secondary, session);
    } else {
        // Restart the primary.
        rst.restart(primary);
        rst.stepUp(primary);
        rst.waitForState(primary, ReplSetTest.State.PRIMARY);
        rst.awaitSecondaryNodes(null, [secondary]);
        // This is needed after restarting the node because the 'lastCommitted' is not set until
        // receiving heartbeats. We need to wait for the 'lastCommitted' OpTime to be past the
        // 'prepareTimestamp' before committing the prepared transaction.
        PrepareHelpers.awaitMajorityCommitted(rst, prepareTimestamp);
    }

    const newPrimary = rst.getPrimary();
    assert.eq(failover ? secondary : primary, newPrimary, "Wrong primary");

    if (commit) {
        // Commit the prepared transaction on the new primary.
        assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));

        // Verify the effect of the transaction.
        const doc = newPrimary.getDB(dbName).getCollection(collName).findOne({});
        assert.docEq(newDoc, doc);
        oldDoc = newDoc;
    } else {
        // Abort the prepared transaction on the new primary.
        assert.commandWorked(session.abortTransaction_forTesting());

        // Verify the effect of the transaction is not applied.
        const doc = newPrimary.getDB(dbName).getCollection(collName).findOne({});
        assert.docEq(oldDoc, doc);
    }

    primary = rst.getPrimary();
    secondary = rst.getSecondary();
};

[false, true].forEach((failover) => [true, false].forEach((commit) => runTest(failover, commit)));

rst.stopSet();
