/**
 * Confirm that replica members undergoing initial sync fail if an invalid index specification is
 * encountered (where index version is >= 2).
 * @tags: [live_record_incompatible]
 */
load("jstests/libs/logv2_helpers.js");

(function() {
"use strict";

// Skip db hash check because of invalid index spec.
TestData.skipCheckDBHashes = true;

load("jstests/replsets/rslib.js");

const testName = "initial_sync_invalid_index_spec";
const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.initiate();

let primaryDB = replTest.getPrimary().getDB(testName);

// Create a V2 index with invalid spec field.
primaryDB.adminCommand(
    {configureFailPoint: "skipIndexCreateFieldNameValidation", mode: "alwaysOn"});
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: "test", indexes: [{v: 2, name: "x_1", key: {x: 1}, invalidOption: 1}]}));

// Add another node to the replica set to allow an initial sync to occur.
var initSyncNode = replTest.add({rsConfig: {votes: 0, priority: 0}});
var initSyncNodeAdminDB = initSyncNode.getDB("admin");

clearRawMongoProgramOutput();
reInitiateWithoutThrowingOnAbortedMember(replTest);

assert.soon(
    function() {
        try {
            initSyncNodeAdminDB.runCommand({ping: 1});
        } catch (e) {
            return true;
        }
        return false;
    },
    "Node did not terminate due to invalid index spec during initial sync",
    ReplSetTest.kDefaultTimeoutMS);

replTest.stop(initSyncNode, undefined, {allowedExitCode: MongoRunner.EXIT_ABRUPT});

const msgInvalidOption = "The field 'invalidOption' is not valid for an index specification";
const msgInitialSyncFatalAssertion = "Fatal assertion 40088 InitialSyncFailure";

if (isJsonLogNoConn()) {
    assert(
        rawMongoProgramOutput().search(
            /Fatal assertion*40088.*InitialSyncFailure.*The field 'invalidOption' is not valid for an index specification/),
        "Replication should have aborted on invalid index specification");
} else {
    assert(rawMongoProgramOutput().match(msgInvalidOption) &&
               rawMongoProgramOutput().match(msgInitialSyncFatalAssertion),
           "Initial sync should have aborted on invalid index specification");
}

replTest.stopSet();
})();
