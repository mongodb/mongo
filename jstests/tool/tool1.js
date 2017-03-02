// bongo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = BongoRunner.dataPath + baseName + "/";
externalPath = BongoRunner.dataPath + baseName + "_external/";
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

var m = BongoRunner.runBongod({dbpath: dbPath, noprealloc: "", bind_ip: "127.0.0.1"});
c = m.getDB(baseName).getCollection(baseName);
c.save({a: 1});
assert(c.findOne());

var exitCode = BongoRunner.runBongoTool("bongodump", {
    host: "127.0.0.1:" + m.port,
    out: externalPath,
});
assert.eq(0, exitCode, "bongodump failed to dump data from bongod");

c.drop();

exitCode = BongoRunner.runBongoTool("bongorestore", {
    host: "127.0.0.1:" + m.port,
    dir: externalPath,
});
assert.eq(0, exitCode, "bongorestore failed to restore data to bongod");

assert.soon("c.findOne()", "bongodump then restore has no data w/sleep");
assert(c.findOne(), "bongodump then restore has no data");
assert.eq(1, c.findOne().a, "bongodump then restore has no broken data");

resetDbpath(externalPath);

assert.eq(-1, fileSize(), "bongoexport prep invalid");

exitCode = BongoRunner.runBongoTool("bongoexport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    out: externalFile,
});
assert.eq(
    0, exitCode, "bongoexport failed to export collection '" + c.getFullName() + "' from bongod");

assert.lt(10, fileSize(), "file size changed");

c.drop();

exitCode = BongoRunner.runBongoTool("bongoimport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    file: externalFile,
});
assert.eq(
    0, exitCode, "bongoimport failed to import collection '" + c.getFullName() + "' into bongod");

assert.soon("c.findOne()", "bongo import json A");
assert(c.findOne() && 1 == c.findOne().a, "bongo import json B");
