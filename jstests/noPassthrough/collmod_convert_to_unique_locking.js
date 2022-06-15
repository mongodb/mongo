/**
 * Tests that the collMod command converting an index to unique fails if the index is modified
 * between the lock acquisitions.
 *
 * @tags: [
 *  # TODO(SERVER-61182): Fix WiredTigerKVEngine::alterIdentMetadata() under inMemory.
 *  requires_persistence,
 *  requires_replication,
 * ]
 */

(function() {
'use strict';

load('jstests/libs/fail_point_util.js');
load('jstests/libs/parallel_shell_helpers.js');

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const collModIndexUniqueEnabled =
    assert.commandWorked(primary.adminCommand({getParameter: 1, featureFlagCollModIndexUnique: 1}))
        .featureFlagCollModIndexUnique.value;

if (!collModIndexUniqueEnabled) {
    jsTestLog('Skipping test because the collMod unique index feature flag is disabled');
    rst.stopSet();
    return;
}

const testDB = primary.getDB('test');
const collName = "collmod_convert_to_unique_locking";
const coll = testDB.getCollection(collName);

assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insert({a: 1}));

// Disallows new duplicate keys on the index.
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));

let awaitCollMod = () => {};
const failPoint = configureFailPoint(
    primary, 'hangAfterCollModIndexUniqueReleaseIXLock', {nss: coll.getFullName()});

// Attempts to run collMod to convert the index to unique during which the index is modified.
awaitCollMod = assertCommandFailedWithCodeInParallelShell(
    primary,
    testDB,
    {collMod: collName, index: {keyPattern: {a: 1}, unique: true}},
    ErrorCodes.CommandFailed);

failPoint.wait();

// Invalidates the index being converted to unique to fail the command.
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, hidden: true}}));

failPoint.off();
awaitCollMod();

// Reruns the collmod without the index being modified and succeeds.
assert.commandWorked(
    testDB.runCommand({collMod: collName, index: {keyPattern: {a: 1}, unique: true}}));

rst.stopSet();
})();
