
if ( jsTest.options().storageEngine &&
     jsTest.options().storageEngine.toLowerCase() == "wiredtiger" ) {

    var baseDir = "jstests_split_c_and_i";
    port = allocatePorts( 1 )[ 0 ];
    dbpath = MongoRunner.dataPath + baseDir + "/";

    var m = startMongodTest(port, baseDir, false, {wiredTigerDirectoryForIndexes : ""} );
    db = m.getDB( "foo" );
    db.bar.insert( { x : 1 } );
    assert.eq( 1, db.bar.count() );

    db.adminCommand( {fsync:1} );

    assert( listFiles( dbpath + "/index" ).length > 0 );
    assert( listFiles( dbpath + "/collection" ).length > 0 );
}
