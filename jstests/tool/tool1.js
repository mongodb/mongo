// mongo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = MongoRunner.dataPath + baseName + "/";
externalPath = MongoRunner.dataPath + baseName + "_external/";
externalBaseName = "export.json";
externalFile = externalPath + externalBaseName;

function fileSize() {
    var l = listFiles(externalPath);
    for (var i = 0; i < l.length; i++) {
        if (l[i].baseName == externalBaseName)
            return l[i].size;
    }
    return -1;
}

resetDbpath(externalPath);

var m = MongoRunner.runMongod({dbpath: dbPath, noprealloc: "", bind_ip: "127.0.0.1"});
c = m.getDB(baseName).getCollection(baseName);
c.save({a: 1});
assert(c.findOne());

var exitCode = MongoRunner.runMongoTool("mongodump", {
    host: "127.0.0.1:" + m.port,
    out: externalPath,
});
assert.eq(0, exitCode, "mongodump failed to dump data from mongod");

c.drop();

exitCode = MongoRunner.runMongoTool("mongorestore", {
    host: "127.0.0.1:" + m.port,
    dir: externalPath,
});
assert.eq(0, exitCode, "mongorestore failed to restore data to mongod");

assert.soon("c.findOne()", "mongodump then restore has no data w/sleep");
assert(c.findOne(), "mongodump then restore has no data");
assert.eq(1, c.findOne().a, "mongodump then restore has no broken data");

resetDbpath(externalPath);

assert.eq(-1, fileSize(), "mongoexport prep invalid");

exitCode = MongoRunner.runMongoTool("mongoexport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    out: externalFile,
});
assert.eq(
    0, exitCode, "mongoexport failed to export collection '" + c.getFullName() + "' from mongod");

assert.lt(10, fileSize(), "file size changed");

c.drop();

exitCode = MongoRunner.runMongoTool("mongoimport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    file: externalFile,
});
assert.eq(
    0, exitCode, "mongoimport failed to import collection '" + c.getFullName() + "' into mongod");

assert.soon("c.findOne()", "mongo import json A");
assert(c.findOne() && 1 == c.findOne().a, "mongo import json B");
