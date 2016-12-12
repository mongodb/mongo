// SERVER-594 test

var baseName = "jstests_disk_newcollection";
var m = MongoRunner.runMongod({noprealloc: "", smallfiles: ""});
db = m.getDB("test");

var t = db[baseName];
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

for (var pass = 0; pass <= 1; pass++) {
    db.createCollection(baseName, {size: 15.8 * 1024 * 1024});
    if (pass == 0)
        t.drop();

    size = getTotalNonLocalNonAdminSize();
    t.save({});
    assert.eq(size, getTotalNonLocalNonAdminSize());
    assert(size <= 32 * 1024 * 1024);

    t.drop();
}
