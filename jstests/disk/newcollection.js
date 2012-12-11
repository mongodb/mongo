// SERVER-594 test

port = allocatePorts( 1 )[ 0 ]
var baseName = "jstests_disk_newcollection";
var m = startMongod( "--noprealloc", "--smallfiles", "--port", port, "--dbpath", "/data/db/" + baseName );
//var m = db.getMongo();
db = m.getDB( "test" );

var t = db[baseName];
var getTotalNonLocalSize = function() {
    var totalNonLocalDBSize = 0;
    m.getDBs().databases.forEach( function(dbStats) {
            if (dbStats.name != "local")
                totalNonLocalDBSize += dbStats.sizeOnDisk;
    });
    return totalNonLocalDBSize;
}

for (var pass = 0; pass <= 1; pass++) {

    db.createCollection(baseName, { size: 15.8 * 1024 * 1024 });
    if( pass == 0 )
        t.drop();

    size = getTotalNonLocalSize();
    t.save({});
    assert.eq(size, getTotalNonLocalSize());
    assert(size <= 32 * 1024 * 1024);

    t.drop();
}
