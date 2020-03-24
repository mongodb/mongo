/**
 * Confirm that replication of an invalid index specification causes server abort (where index
 * version is >= 2).
 */

(function() {
"use strict";

load("jstests/replsets/rslib.js");
load("jstests/libs/logv2_helpers.js");

const testName = "invalid_index_spec";
const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
replTest.initiate();

let primaryDB = replTest.getPrimary().getDB(testName);
let secondary = replTest.getSecondary();
let secondaryAdminDB = secondary.getDB("admin");

// Set a fail point that allows for index creation with invalid spec fields.
primaryDB.adminCommand(
    {configureFailPoint: "skipIndexCreateFieldNameValidation", mode: "alwaysOn"});

clearRawMongoProgramOutput();

// Create a V1 index with invalid spec field. Expected to replicate without error or server
// abort.
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: "test", indexes: [{v: 1, name: "w_1", key: {w: 1}, invalidOption1: 1}]}));

// Create a V2 index with invalid spec field. Expected to cause server abort on replication.
assert.commandWorked(primaryDB.runCommand(
    {createIndexes: "test", indexes: [{v: 2, name: "x_1", key: {x: 1}, invalidOption2: 1}]}));

assert.soon(function() {
    try {
        secondaryAdminDB.runCommand({ping: 1});
    } catch (e) {
        return true;
    }
    return false;
}, "Node did not terminate due to invalid index spec", 60 * 1000);

// fassert() calls std::abort(), which returns a different exit code for Windows vs. other
// platforms.
const exitCode = MongoRunner.EXIT_ABRUPT;
replTest.stop(secondary, undefined, {allowedExitCode: exitCode});

// During the transition from the old code path in IndexBuilder to IndexBuildsCoordinator, we
// will accept the fatal assertion code from either component.
const msgIndexBuilder = "Fatal assertion 50769";
const msgIndexBuildsCoordinator = "Fatal assertion 34437";
const msgIndexErrorType = "InvalidIndexSpecificationOption";
const msgIndexError = "The field 'invalidOption2'";

if (isJsonLogNoConn()) {
    assert(
        rawMongoProgramOutput().search(
            /Fatal assertion.*(50769|34437).*InvalidIndexSpecificationOption.*The field 'invalidOption2'/),
        "Replication should have aborted on invalid index specification");
} else {
    assert((rawMongoProgramOutput().match(msgIndexBuilder) ||
            rawMongoProgramOutput().match(msgIndexBuildsCoordinator)) &&
               (rawMongoProgramOutput().match(msgIndexErrorType) &&
                rawMongoProgramOutput().match(msgIndexError)),
           "Replication should have aborted on invalid index specification");
}
replTest.stopSet();
})();
