/*
 * Tests that if a client sends "helloOk: true" as a part of their isMaster request,
 * mongod will replace "not master" error messages with "not primary".
 *
 * In practice, drivers will send "helloOk: true" in the initial handshake when
 * opening a connection to the database.
 */

(function() {
"use strict";

const dbName = "test";
const collName = "not_primary_errors_returned_if_client_sends_helloOk";
const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const primary = rst.getPrimary();
const secondary = rst.getSecondary();

assert.commandWorked(primary.getDB(dbName)[collName].insert({x: 1}));
rst.awaitReplication();

// Attempts to write to a secondary should fail with NotWritablePrimary.
let res = assert.commandFailedWithCode(secondary.getDB(dbName)[collName].insert({y: 2}),
                                       ErrorCodes.NotWritablePrimary,
                                       "insert did not fail with NotWritablePrimary");
// Since the shell opens connections without using "helloOk: true", the error message
// should include "not master".
assert(res.errmsg.includes("not master"), res);

// Set secondaryOk to false, disallowing reads on secondaries.
secondary.getDB(dbName).getMongo().setSecondaryOk(false);
assert.eq(secondary.getDB(dbName).getMongo().getSecondaryOk(), false);
res = assert.commandFailedWithCode(secondary.getDB(dbName).runCommand({find: collName}),
                                   ErrorCodes.NotPrimaryNoSecondaryOk,
                                   "find did not fail with NotPrimaryNoSecondaryOk");
// Since we did not send "helloOk: true", the error message should include "not master".
assert(res.errmsg.includes("not master"), res);

// An isMaster response will not contain "helloOk: true" if the client does not send
// helloOk in the request.
res = assert.commandWorked(secondary.getDB(dbName).adminCommand({isMaster: 1}));
assert.eq(res.helloOk, undefined);

try {
    // "helloOk" field type must be a boolean.
    secondary.getDB(dbName).adminCommand({isMaster: 1, helloOk: 3});
} catch (e) {
    assert.eq(e.code, ErrorCodes.TypeMismatch, "invalid helloOk field did not fail to parse");
}

// Run the isMaster command with "helloOk: true" on the secondary.
res = assert.commandWorked(secondary.getDB(dbName).adminCommand({isMaster: 1, helloOk: true}));
// The response should contain "helloOk: true", which indicates to the client that the
// server supports the hello command.
assert.eq(res.helloOk, true);

// Attempts to write to a secondary should fail with NotWritablePrimary.
res = assert.commandFailedWithCode(secondary.getDB(dbName)[collName].insert({z: 3}),
                                   ErrorCodes.NotWritablePrimary,
                                   "insert did not fail with NotWritablePrimary");
// Since we sent "helloOk: true", the error message should include "not primary".
assert(res.errmsg.includes("not primary"), res);
assert(!res.errmsg.includes("not master"), res);

// secondaryOk was already set to false, so the following read should still fail with
// NotPrimaryNoSecondaryOk.
assert.eq(secondary.getDB(dbName).getMongo().getSecondaryOk(), false);
res = assert.commandFailedWithCode(secondary.getDB(dbName).runCommand({find: collName}),
                                   ErrorCodes.NotPrimaryNoSecondaryOk,
                                   "find did not fail with NotPrimaryNoSecondaryOk");
// Since we sent "helloOk: true", the error message should include "not primary".
assert(res.errmsg.includes("not primary"), res);
assert(!res.errmsg.includes("not master"), res);

rst.stopSet();
})();
