// test --repairpath on another partition

var baseName = "jstests_disk_repair3";
var repairbase = MongoRunner.dataDir + "/repairpartitiontest";
var repairpath = repairbase + "/dir";

doIt = false;
files = listFiles(MongoRunner.dataDir);
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
    var dbpath = MongoRunner.dataPath + baseName + "/";

    resetDbpath(dbpath);
    resetDbpath(repairpath);

    var m = MongoRunner.runMongod({
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
    MongoRunner.stopMongod(m.port);

    resetDbpath(repairpath);
    var rc = runMongoProgram("mongod",
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
    m = MongoRunner.runMongod({
        nssize: 8,
        noprealloc: "",
        smallfiles: "",
        port: m.port,
        dbpath: dbpath,
        repairpath: repairpath,
    });
    db = m.getDB(baseName);
    check();
    MongoRunner.stopMongod(m.port);
}
