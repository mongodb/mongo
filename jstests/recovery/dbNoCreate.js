var baseName = "jstests_dbNoCreate";

var m = startMongod( "--port", "27018", "--dbpath", "/data/db/" + baseName );

var t = m.getDB( baseName ).t;

var no = function( dbName ) {
    assert.eq( -1, db.getMongo().getDBNames().indexOf( dbName ) );    
}

assert.eq( 0, t.find().toArray().length );
t.remove();
t.update( {}, { a:1 } );
t.drop();

stopMongod( 27018 );

var m = startMongoProgram( "mongod", "--port", "27018", "--dbpath", "/data/db/" + baseName );
assert.eq( -1, m.getDBNames().indexOf( baseName ) );
