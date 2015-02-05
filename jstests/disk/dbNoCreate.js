var baseName = "jstests_dbNoCreate";

var m = startMongod( "--port", "27018", "--dbpath", MongoRunner.dataPath + baseName );

var t = m.getDB( baseName ).t;

assert.eq( 0, t.find().toArray().length );
t.remove({});
t.update( {}, { a:1 } );
t.drop();

stopMongod( 27018 );

var m = startMongoProgram( "mongod", "--port", "27018", "--dbpath", MongoRunner.dataPath + baseName );
assert.eq( -1, 
           m.getDBNames().indexOf( baseName ), 
           "found " + baseName + " in " + tojson(m.getDBNames()));
