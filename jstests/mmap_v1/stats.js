
var statsDB = db.getSiblingDB("stats");
statsDB.dropDatabase();
var t = statsDB.stats1;

t.save({a: 1});

assert.lt(0, t.dataSize(), "A");
assert.lt(t.dataSize(), t.storageSize(), "B");
assert.lt(0, t.totalIndexSize(), "C");

var stats = statsDB.stats();
assert.gt(stats.fileSize, 0);
assert.eq(stats.dataFileVersion.major, 4);
assert.eq(stats.dataFileVersion.minor, 22);

// test empty database; should be no dataFileVersion
statsDB.dropDatabase();
var statsEmptyDB = statsDB.stats();
assert.eq(statsEmptyDB.fileSize, 0);
assert.isnull(statsEmptyDB.dataFileVersion);

statsDB.dropDatabase();
