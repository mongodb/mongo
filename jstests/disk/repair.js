// check --repairpath and --repair

// `--repairpath` is mmap only.
// @tags: [requires_mmapv1]

var baseName = "jstests_disk_repair";
var dbpath = BongoRunner.dataPath + baseName + "/";
var repairpath = dbpath + "repairDir/";

resetDbpath(dbpath);
resetDbpath(repairpath);

var m = BongoRunner.runBongod({
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
BongoRunner.stopBongod(m.port);

resetDbpath(repairpath);
m = BongoRunner.runBongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
assert.commandWorked(db.runCommand({repairDatabase: 1}));
check();
BongoRunner.stopBongod(m.port);

resetDbpath(repairpath);
rc = runBongoProgram(
    "bongod", "--repair", "--port", m.port, "--dbpath", dbpath, "--repairpath", repairpath);
assert.eq.automsg("0", "rc");
m = BongoRunner.runBongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
BongoRunner.stopBongod(m.port);

resetDbpath(repairpath);
rc = runBongoProgram("bongod", "--repair", "--port", m.port, "--dbpath", dbpath);
assert.eq.automsg("0", "rc");
m = BongoRunner.runBongod({
    port: m.port,
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
