// test --repairpath on another partition

// `--repairpath` is mmap only.
// @tags: [requires_mmapv1]

var baseName = "jstests_disk_repair3";
var repairbase = BongoRunner.dataDir + "/repairpartitiontest";
var repairpath = repairbase + "/dir";

doIt = false;
files = listFiles(BongoRunner.dataDir);
for (i in files) {
    if (files[i].name == repairbase) {
        doIt = true;
    }
}

if (!doIt) {
    print("path " + repairpath + " missing, skipping repair3 test");
    doIt = false;
}

if (doIt) {
    var dbpath = BongoRunner.dataPath + baseName + "/";

    resetDbpath(dbpath);
    resetDbpath(repairpath);

    var m = BongoRunner.runBongod({
        nssize: 8,
        noprealloc: "",
        smallfiles: "",
        dbpath: dbpath,
        repairpath: repairpath,
    });
    db = m.getDB(baseName);
    db[baseName].save({});
    assert.commandWorked(db.runCommand({repairDatabase: 1, backupOriginalFiles: false}));
    function check() {
        files = listFiles(dbpath);
        for (f in files) {
            assert(!new RegExp("^" + dbpath + "backup_").test(files[f].name),
                   "backup dir in dbpath");
        }

        assert.eq.automsg("1", "db[ baseName ].count()");
    }

    check();
    BongoRunner.stopBongod(m.port);

    resetDbpath(repairpath);
    var rc = runBongoProgram("bongod",
                             "--nssize",
                             "8",
                             "--noprealloc",
                             "--smallfiles",
                             "--repair",
                             "--port",
                             m.port,
                             "--dbpath",
                             dbpath,
                             "--repairpath",
                             repairpath);
    assert.eq.automsg("0", "rc");
    m = BongoRunner.runBongod({
        nssize: 8,
        noprealloc: "",
        smallfiles: "",
        port: m.port,
        dbpath: dbpath,
        repairpath: repairpath,
    });
    db = m.getDB(baseName);
    check();
    BongoRunner.stopBongod(m.port);
}
