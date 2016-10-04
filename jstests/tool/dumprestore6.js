// Test restoring from a dump with v:0 indexes.
// mongodump strips the 'v' property from the index specification by default.  When using
// --keepIndexVersion, the 'v' property is not stripped, but index creation will fail.

var toolTest = new ToolTest("dumprestore6");
var col = toolTest.startDB("foo");
var testDb = toolTest.db;
assert.eq(0, col.count(), "setup1");

// Normal restore should succeed and convert v:1 index.
toolTest.runTool(
    "restore", "--dir", "jstests/tool/data/dumprestore6", "--db", "jstests_tool_dumprestore6");
assert.soon("col.findOne()", "no data after sleep");
assert.eq(1, col.count(), "after restore");
var indexes = col.getIndexes();
assert.eq(2, indexes.length, "there aren't the correct number of indexes");

// Try with --keepIndexVersion, should fail to restore v:0 index.
testDb.dropDatabase();
assert.eq(0, col.count(), "after drop");
toolTest.runTool("restore",
                 "--dir",
                 "jstests/tool/data/dumprestore6",
                 "--db",
                 "jstests_tool_dumprestore6",
                 "--keepIndexVersion");
indexes = col.getIndexes();
assert.eq(1, indexes.length, "there aren't the correct number of indexes");

toolTest.stop();
