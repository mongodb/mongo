#!/usr/bin/mongod

baseName = "jstests_shellspawn";
t = db.getSiblingDB('test').getCollection( baseName );
t.drop();

if ( typeof( _startMongoProgram ) == "undefined" ){
    print( "no fork support" );
}
else {
    var evalString = "sleep( 2000 ); db.getSiblingDB('test').getCollection( '" + baseName + "' ).save( {a:1} );";
    spawn = startMongoProgramNoConnect( "mongo", "admin", "--port", myPort(), "--eval", evalString );

//    assert.soon( function() { return 1 == t.count(); } );
    // SERVER-2784 debugging - error message overwritten to indicate last count value.
    assert.soon( "count = t.count(); msg = 'did not reach expected count, last value: ' + t.count(); 1 == count;" );
    
    stopMongoProgramByPid( spawn );
    
    spawn = startMongoProgramNoConnect( "mongo", "--port", myPort(), "--eval", "print( 'I am a shell' );" );
    
    stopMongoProgramByPid( spawn );

    spawn = startMongoProgramNoConnect( "mongo", "--port", myPort() );

    stopMongoProgramByPid( spawn );
    
    spawn = startMongoProgramNoConnect( "mongo", "--port", myPort() );
    
    stopMongoProgramByPid( spawn );

    // all these shells should be killed
}
