// check --repairpath and --repair

var baseName = "jstests_disk_repair";
var dbpath = MongoRunner.dataPath + baseName + "/";
var repairpath = dbpath + "repairDir/";

resetDbpath(dbpath);
resetDbpath(repairpath);

var m = MongoRunner.runMongod({
    dbpath: dbpath,
    repairpath: repairpath,
    noCleanData: true,
});
db = m.getDB(baseName);
db[baseName].save({});
assert.commandWorked(db.runCommand({repairDatabase: 1, backupOriginalFiles: true}));
function check() {
    files = listFiles(dbpath);
    for (f in files) {
        assert(!new RegExp("^" + dbpath + "backup_").test(files[f].name), "backup dir in dbpath");
    }

    assert.eq.automsg("1", "db[ baseName ].count()");
}
check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
m = MongoRunner.runMongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
assert.commandWorked(db.runCommand({repairDatabase: 1}));
check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
rc = runMongoProgram(
    "mongod", "--repair", "--port", m.port, "--dbpath", dbpath, "--repairpath", repairpath);
assert.eq.automsg("0", "rc");
m = MongoRunner.runMongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
rc = runMongoProgram("mongod", "--repair", "--port", m.port, "--dbpath", dbpath);
assert.eq.automsg("0", "rc");
m = MongoRunner.runMongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
