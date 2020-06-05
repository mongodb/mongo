/**
 * Verifies that collections cannot be renamed to a new name over 255 characters.
 *
 * V4.2 binaries do not return an error code in the 'renameCollection' command, only {ok: 0}.
 * @tags: [requires_fcv_44]
 */
(function() {
var replSetName = "rename_collection_long";
const replTest = new ReplSetTest({name: replSetName, nodes: 2});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const db = primary.getDB("test");

assert.commandWorked(db.createCollection("a"));

const longCollName = "1".repeat(512);
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: "test.a", to: "test." + longCollName}),
    [4862100, ErrorCodes.InvalidNamespace]);
assert.commandFailedWithCode(
    db.adminCommand({renameCollection: "test.a", to: "test2." + longCollName}),
    [4862100, ErrorCodes.InvalidNamespace]);

replTest.stopSet();
}());
