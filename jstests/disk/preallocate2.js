// check that there is preallocation on insert

var baseName = "jstests_preallocate2";

var m = MongoRunner.runMongod({});

m.getDB(baseName)[baseName].save({i: 1});

// Windows does not currently use preallocation
expectedMB = (_isWindows() ? 70 : 100);
if (m.getDB(baseName).serverBits() < 64)
    expectedMB /= 4;

assert.soon(function() {
    return m.getDBs().totalSize > expectedMB * 1000000;
}, "\n\n\nFAIL preallocate.js expected second file to bring total size over " + expectedMB + "MB");
