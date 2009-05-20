// mongo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = "/data/db/" + baseName;
externalPath = "/data/db/" + baseName + "/external"
externalFile = externalPath + "/export.json"


port = allocatePorts( 1 )[ 0 ];

m = startMongod( "--port", port, "--dbpath", dbPath, "--nohttpinterface" );
resetDbpath( externalPath );
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
