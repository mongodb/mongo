// check that there is preallocation, and there are 2 files

var baseName = "jstests_preallocate";

var m = MongoRunner.runMongod({});

var getTotalNonLocalNonAdminSize = function() {
    var totalNonLocalNonAdminDBSize = 0;
    m.getDBs().databases.forEach(function(dbStats) {
        // We accept the local database's and admin database's space overhead.
        if (dbStats.name == "local" || dbStats.name == "admin")
            return;

        // Databases with "sizeOnDisk=1" and "empty=true" dont' actually take up space o disk.
        // See SERVER-11051.
        if (dbStats.sizeOnDisk == 1 && dbStats.empty)
            return;
        totalNonLocalNonAdminDBSize += dbStats.sizeOnDisk;
    });
    return totalNonLocalNonAdminDBSize;
};

assert.eq(0, getTotalNonLocalNonAdminSize());

m.getDB(baseName).createCollection(baseName + "1");

// Windows does not currently use preallocation
expectedMB = 64 + 16;
if (m.getDB(baseName).serverBits() < 64)
    expectedMB /= 4;

assert.soon(function() {
    return getTotalNonLocalNonAdminSize() >= expectedMB * 1024 * 1024;
}, "\n\n\nFAIL preallocate.js expected second file to bring total size over " + expectedMB + "MB");

MongoRunner.stopMongod(m);

m = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: m.dbpath});

size = getTotalNonLocalNonAdminSize();

m.getDB(baseName).createCollection(baseName + "2");

sleep(2000);  // give prealloc a chance

assert.eq(size, getTotalNonLocalNonAdminSize());
