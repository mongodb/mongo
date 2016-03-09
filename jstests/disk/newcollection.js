// SERVER-594 test

var baseName = "jstests_disk_newcollection";
var m = MongoRunner.runMongod({noprealloc: "", smallfiles: ""});
db = m.getDB("test");

var t = db[baseName];
var getTotalNonLocalSize = function() {
    var totalNonLocalDBSize = 0;
    m.getDBs().databases.forEach(function(dbStats) {
        // We accept the local database's space overhead.
        if (dbStats.name == "local")
            return;

        // Databases with "sizeOnDisk=1" and "empty=true" dont' actually take up space o disk.
        // See SERVER-11051.
        if (dbStats.sizeOnDisk == 1 && dbStats.empty)
            return;
        totalNonLocalDBSize += dbStats.sizeOnDisk;
    });
    return totalNonLocalDBSize;
};

for (var pass = 0; pass <= 1; pass++) {
    db.createCollection(baseName, {size: 15.8 * 1024 * 1024});
    if (pass == 0)
        t.drop();

    size = getTotalNonLocalSize();
    t.save({});
    assert.eq(size, getTotalNonLocalSize());
    assert(size <= 32 * 1024 * 1024);

    t.drop();
}
