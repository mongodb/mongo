// mongo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = "/data/db/" + baseName + "/";
externalPath = "/data/db/" + baseName + "_external/"
externalFile = externalPath + "export.json"

port = allocatePorts( 1 )[ 0 ];
resetDbpath( externalPath );

m = startMongod( "--port", port, "--dbpath", dbPath, "--nohttpinterface" );
c = m.getDB( baseName ).getCollection( baseName );
c.save( { a: 1 } );

startMongoProgramNoConnect( "mongodump", "--host", "127.0.0.1:" + port, "--out", externalPath );
sleep( 3000 );
c.drop();
startMongoProgramNoConnect( "mongorestore", "--host", "127.0.0.1:" + port, "--dir", externalPath );
assert.soon( function() { return c.findOne() && 1 == c.findOne().a; } );

resetDbpath( externalPath );

startMongoProgramNoConnect( "mongoexport", "--host", "127.0.0.1:" + port, "-d", baseName, "-c", baseName, "--out", externalFile );
sleep( 3000 );
c.drop();
startMongoProgramNoConnect( "mongoimportjson", "--host", "127.0.0.1:" + port, "-d", baseName, "-c", baseName, "--file", externalFile );
assert.soon( function() { return c.findOne() && 1 == c.findOne().a; } );

stopMongod( port );
resetDbpath( externalPath );

startMongoProgramNoConnect( "mongodump", "--dbpath", dbPath, "--out", externalPath );
sleep( 3000 );
resetDbpath( dbPath );
startMongoProgramNoConnect( "mongorestore", "--dbpath", dbPath, "--dir", externalPath );
sleep( 5000 );
m = startMongoProgram( "mongod", "--port", port, "--dbpath", dbPath, "--nohttpinterface" );
c = m.getDB( baseName ).getCollection( baseName );
assert( c.findOne(), "object missing" );
assert( 1 == c.findOne().a, "object wrong" );

stopMongod( port );
resetDbpath( externalPath );

startMongoProgramNoConnect( "mongoexport", "--dbpath", dbPath, "-d", baseName, "-c", baseName, "--out", externalFile );
sleep( 3000 );
resetDbpath( dbPath );
startMongoProgramNoConnect( "mongoimportjson", "--dbpath", dbPath, "-d", baseName, "-c", baseName, "--file", externalFile );
sleep( 5000 );
m = startMongoProgram( "mongod", "--port", port, "--dbpath", dbPath, "--nohttpinterface" );
c = m.getDB( baseName ).getCollection( baseName );
assert( c.findOne(), "object missing" );
assert( 1 == c.findOne().a, "object wrong" );
