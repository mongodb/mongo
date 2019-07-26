/**
 * Verify that speculative majority is only allowed on supported commands.
 *
 * Currently, only change stream aggregation commands and the 'find' command with the
 * 'allowSpeculativeMajorityRead' flag are permitted.
 *
 * @tags: [uses_speculative_majority]
 */
(function() {
"use strict";

let name = "speculative_majority_supported_commands";
let replTest =
    new ReplSetTest({name: name, nodes: 1, nodeOptions: {enableMajorityReadConcern: 'false'}});
replTest.startSet();
replTest.initiate();

let dbName = name;
let collName = "coll";

let primary = replTest.getPrimary();
let primaryDB = primary.getDB(dbName);

// Create a collection.
assert.commandWorked(primaryDB[collName].insert({_id: 0}, {writeConcern: {w: "majority"}}));

/**
 * Allowed commands.
 */

// Change stream aggregation is allowed.
let res = primaryDB.runCommand({
    aggregate: collName,
    pipeline: [{$changeStream: {}}],
    cursor: {},
    readConcern: {level: "majority"}
});
assert.commandWorked(res);

// Find query with speculative flag is allowed.
res = primaryDB.runCommand(
    {find: collName, readConcern: {level: "majority"}, allowSpeculativeMajorityRead: true});
assert.commandWorked(res);

/**
 * Disallowed commands.
 */

// A non change stream aggregation is not allowed.
res = primaryDB.runCommand({
    aggregate: collName,
    pipeline: [{$project: {}}],
    cursor: {},
    readConcern: {level: "majority"}
});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);

// The 'find' command without requisite flag is unsupported.
res = primaryDB.runCommand({find: collName, readConcern: {level: "majority"}});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);

res = primaryDB.runCommand(
    {find: collName, readConcern: {level: "majority"}, allowSpeculativeMajorityRead: false});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);

// Another basic read command. We don't exhaustively check all commands.
res = primaryDB.runCommand({count: collName, readConcern: {level: "majority"}});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);

// Speculative flag is only allowed on find commands.
res = primaryDB.runCommand(
    {count: collName, readConcern: {level: "majority"}, allowSpeculativeMajorityRead: true});
assert.commandFailedWithCode(res, ErrorCodes.ReadConcernMajorityNotEnabled);

replTest.stopSet();
})();