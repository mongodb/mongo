
if ( jsTest.options().storageEngine == "wiredTiger" ) {

    var baseDir = "jstests_per_db_and_split_c_and_i";
    port = allocatePorts( 1 )[ 0 ];
    dbpath = MongoRunner.dataPath + baseDir + "/";

    var m = MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        wiredTigerDirectoryForIndexes: '',
        directoryperdb: ''});
    db = m.getDB( "foo" );
    db.bar.insert( { x : 1 } );
    assert.eq( 1, db.bar.count() );

    db.adminCommand( {fsync:1} );

    assert( listFiles( dbpath + "/foo/index" ).length > 0 );
    assert( listFiles( dbpath + "/foo/collection" ).length > 0 );

    MongoRunner.stopMongod(port);

    // Subsequent attempts to start server using same dbpath but different
    // wiredTigerDirectoryForIndexes and directoryperdb options should fail.
    assert.isnull(MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        restart: true}));
    assert.isnull(MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        restart: true,
        directoryperdb: ''}));
    assert.isnull(MongoRunner.runMongod({
        dbpath: dbpath,
        port: port,
        restart: true,
        wiredTigerDirectoryForIndexes: ''}));
}
