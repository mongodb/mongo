/**
 * Tests that overriding the wiredtiger verify() configuration works.
 *
 * The `requires_persistence` is necessary because we need an actual disk to write WT btrees into in
 * order to inspect their shape.
 * @tags: [
 *     requires_persistence,
 *     requires_wiredtiger,
 * ]
 */

const baseName = jsTestName();
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);
let conn = MongoRunner.runMongod({dbpath: dbpath});
let db = conn.getDB("test");
assert.commandWorked(db.createCollection(baseName));
let coll = db.getCollection(baseName);

coll.insert({a: 1});
coll.insert({a: 2});
coll.insert({a: 3});

// Make the changes visible on disk (for WT verify), and to avoid EBUSYs.
db.adminCommand({fsync: 1});

// Only full validations can override the config (it has no effect otherwise).
assert.commandFailedWithCode(
    coll.validate({wiredtigerVerifyConfigurationOverride: "dump_tree_shape"}),
    ErrorCodes.InvalidOptions);

// Invalid configuration strings
assert.commandFailedWithCode(
    coll.validate({full: true, wiredtigerVerifyConfigurationOverride: "blahblah"}),
    ErrorCodes.InvalidOptions);

// Even strings which are configuration options that aren't in our allowlist.
assert.commandFailedWithCode(
    coll.validate({full: true, wiredtigerVerifyConfigurationOverride: "dump_all_data"}),
    ErrorCodes.InvalidOptions);

// Empty string is allowed.
assert.commandWorked(coll.validate({full: true, wiredtigerVerifyConfigurationOverride: ""}));

clearRawMongoProgramOutput();

// Here's a real one.
assert.commandWorked(
    coll.validate({full: true, wiredtigerVerifyConfigurationOverride: "dump_tree_shape"}));

// Check for the kinds of additional logs that are printed when "dump_tree_shape" is in the config.
const waitForLogsTimeoutMs = 10 * 1000;
assert.soon(() => rawMongoProgramOutput(".*WT_PAGE_ROW_LEAF.*") != "",
            "Log output must show evidence that wiredtiger dumped the tree's shape",
            waitForLogsTimeoutMs);

MongoRunner.stopMongod(conn);
