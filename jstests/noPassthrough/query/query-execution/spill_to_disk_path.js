/**
 * Tests that the spillPath config allows the specification of the spill to disk path.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
// No two spill engines should be using the same path
const path = MongoRunner.dataPath + "spill";
jsTestLog(path);

const conn = MongoRunner.runMongod({setParameter: "spillPath=" + path});
const db = conn.getDB("test");

assert.eq(fileExists(path), true);
assert.eq(fileExists(path + "/WiredTigerHS.wt"), true);
assert.eq(fileExists(path + "/WiredTiger.wt"), true);

MongoRunner.stopMongod(conn);
