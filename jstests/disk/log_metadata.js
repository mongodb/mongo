/**
 * Tests that when WT metadata table is logged when checkApplicationMetadataFormatVersion fails.
 *
 * The `requires_persistence` is necessary because we need an actual disk to write data.
 * @tags: [
 *     requires_persistence,
 *     requires_wiredtiger,
 * ]
 */

import {getUriForIndex, runWiredTigerTool, startMongodOnExistingPath} from "jstests/disk/libs/wt_file_helper.js";

// Because this test intentionally crashes the server, we instruct the
// the shell to clean up after us and remove the core dump.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

const baseName = jsTestName();
const dbpath = MongoRunner.dataPath + baseName + "/";

resetDbpath(dbpath);
let conn = MongoRunner.runMongod({dbpath: dbpath});
let db = conn.getDB("test");
assert.commandWorked(db.createCollection(baseName));

let coll = db.getCollection(baseName);
assert.commandWorked(coll.createIndex({a: 1}, {unique: true}));
let uri = getUriForIndex(coll, "a_1");

// Invalidate WT metadata table.
jsTestLog(`Modifying table: ${uri}`);
MongoRunner.stopMongod(conn, null, {skipValidation: true});
runWiredTigerTool("-h", conn.dbpath, "alter", "table:" + uri, "app_metadata=(abc=123)");

assert.throws(() => {
    conn = startMongodOnExistingPath(dbpath);
});

assert.soon(() => {
    const foundId = rawMongoProgramOutput(".*").match(/\"id\":11504300/);
    const expectedOutput = `${uri}","value":"app_metadata=(abc=123),`;
    const foundExpectedOutput = rawMongoProgramOutput(".*").includes(expectedOutput);
    return foundId && foundExpectedOutput;
}, "Mongod log contained the full contents of the WT metadata table");
