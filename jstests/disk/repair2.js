// repair with --directoryperdb

var baseName = "jstests_disk_repair2";

function check() {
    files = listFiles(dbpath);
    for (f in files) {
        assert(!new RegExp("^" + dbpath + "backup_").test(files[f].name),
               "backup dir " + files[f].name + " in dbpath");
    }

    assert.eq.automsg("1", "db[ baseName ].count()");
}

var dbpath = MongoRunner.dataPath + baseName + "/";
var repairpath = dbpath + "repairDir/";
var longDBName = Array(61).join('a');
var longRepairPath = dbpath + Array(61).join('b') + '/';

resetDbpath(dbpath);
resetDbpath(repairpath);

var m = MongoRunner.runMongod({
    directoryperdb: "",
    dbpath: dbpath,
    repairpath: repairpath,
    noCleanData: true,
});
db = m.getDB(baseName);
db[baseName].save({});
assert.commandWorked(db.runCommand({repairDatabase: 1, backupOriginalFiles: true}));

// Check that repair files exist in the repair directory, and nothing else
db.adminCommand({fsync: 1});
files = listFiles(repairpath + "/backup_repairDatabase_0/" + baseName);
var fileCount = 0;
for (f in files) {
    print(files[f].name);
    if (files[f].isDirectory)
        continue;
    fileCount += 1;
    assert(/\.bak$/.test(files[f].name),
           "In database repair directory, found unexpected file: " + files[f].name);
}
assert(fileCount > 0, "Expected more than zero nondirectory files in the database directory");

check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
assert.commandWorked(db.runCommand({repairDatabase: 1}));
check();
MongoRunner.stopMongod(m.port);

// Test long database names
resetDbpath(repairpath);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(longDBName);
assert.writeOK(db[baseName].save({}));
assert.commandWorked(db.runCommand({repairDatabase: 1}));
MongoRunner.stopMongod(m.port);

// Test long repairPath
resetDbpath(longRepairPath);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    repairpath: longRepairPath,
    noCleanData: true,
});
db = m.getDB(longDBName);
assert.commandWorked(db.runCommand({repairDatabase: 1, backupOriginalFiles: true}));
check();
MongoRunner.stopMongod(m.port);

// Test database name and repairPath with --repair
resetDbpath(longRepairPath);
var returnCode = runMongoProgram("mongod",
                                 "--port",
                                 m.port,
                                 "--repair",
                                 "--directoryperdb",
                                 "--dbpath",
                                 dbpath,
                                 "--repairpath",
                                 longRepairPath);
assert.eq(returnCode, 0);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(longDBName);
check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
returnCode = runMongoProgram("mongod",
                             "--port",
                             m.port,
                             "--repair",
                             "--directoryperdb",
                             "--dbpath",
                             dbpath,
                             "--repairpath",
                             repairpath);
assert.eq(returnCode, 0);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    repairpath: repairpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
MongoRunner.stopMongod(m.port);

resetDbpath(repairpath);
returnCode =
    runMongoProgram("mongod", "--port", m.port, "--repair", "--directoryperdb", "--dbpath", dbpath);
assert.eq(returnCode, 0);
m = MongoRunner.runMongod({
    port: m.port,
    directoryperdb: "",
    dbpath: dbpath,
    noCleanData: true,
});
db = m.getDB(baseName);
check();
