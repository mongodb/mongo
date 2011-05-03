// SERVER-594 test

port = allocatePorts( 1 )[ 0 ]
var baseName = "jstests_disk_newcollection";
var m = startMongod( "--noprealloc", "--smallfiles", "--port", port, "--dbpath", "/data/db/" + baseName );
//var m = db.getMongo();
db = m.getDB( "test" );

var t = db[baseName];

for (var pass = 0; pass <= 1; pass++) {

    db.createCollection(baseName, { size: 15.8 * 1024 * 1024 });
    if( pass == 0 )
        t.drop();

    size = m.getDBs().totalSize;
    t.save({});
    assert.eq(size, m.getDBs().totalSize);
    assert(size <= 32 * 1024 * 1024);

    t.drop();
}
