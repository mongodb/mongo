// merizo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = MerizoRunner.dataPath + baseName + "/";
externalPath = MerizoRunner.dataPath + baseName + "_external/";
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

var m = MerizoRunner.runMerizod({dbpath: dbPath, bind_ip: "127.0.0.1"});
c = m.getDB(baseName).getCollection(baseName);
c.save({a: 1});
assert(c.findOne());

var exitCode = MerizoRunner.runMerizoTool("merizodump", {
    host: "127.0.0.1:" + m.port,
    out: externalPath,
});
assert.eq(0, exitCode, "merizodump failed to dump data from merizod");

c.drop();

exitCode = MerizoRunner.runMerizoTool("merizorestore", {
    host: "127.0.0.1:" + m.port,
    dir: externalPath,
});
assert.eq(0, exitCode, "merizorestore failed to restore data to merizod");

assert.soon("c.findOne()", "merizodump then restore has no data w/sleep");
assert(c.findOne(), "merizodump then restore has no data");
assert.eq(1, c.findOne().a, "merizodump then restore has no broken data");

resetDbpath(externalPath);

assert.eq(-1, fileSize(), "merizoexport prep invalid");

exitCode = MerizoRunner.runMerizoTool("merizoexport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    out: externalFile,
});
assert.eq(
    0, exitCode, "merizoexport failed to export collection '" + c.getFullName() + "' from merizod");

assert.lt(10, fileSize(), "file size changed");

c.drop();

exitCode = MerizoRunner.runMerizoTool("merizoimport", {
    host: "127.0.0.1:" + m.port,
    db: baseName,
    collection: baseName,
    file: externalFile,
});
assert.eq(
    0, exitCode, "merizoimport failed to import collection '" + c.getFullName() + "' into merizod");

assert.soon("c.findOne()", "merizo import json A");
assert(c.findOne() && 1 == c.findOne().a, "merizo import json B");
MerizoRunner.stopMerizod(m);