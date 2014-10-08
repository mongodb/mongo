// mongo tool tests, very basic to start with

baseName = "jstests_tool_tool1";
dbPath = MongoRunner.dataPath + baseName + "/";
externalPath = MongoRunner.dataPath + baseName + "_external/";
externalBaseName = "export.json";
externalFile = externalPath + externalBaseName;

function fileSize(){
    var l = listFiles( externalPath );
    for ( var i=0; i<l.length; i++ ){
        if ( l[i].baseName == externalBaseName )
            return l[i].size;
    }
    return -1;
}


port = allocatePorts( 1 )[ 0 ];
resetDbpath( externalPath );

m = startMongod( "--port", port, "--dbpath", dbPath, "--nohttpinterface", "--noprealloc" , "--bind_ip", "127.0.0.1" );
c = m.getDB( baseName ).getCollection( baseName );
c.save( { a: 1 } );
assert( c.findOne() );

runMongoProgram( "mongodump", "--host", "127.0.0.1:" + port, "--out", externalPath );
c.drop();
runMongoProgram( "mongorestore", "--host", "127.0.0.1:" + port, "--dir", externalPath );
assert.soon( "c.findOne()" , "mongodump then restore has no data w/sleep" );
assert( c.findOne() , "mongodump then restore has no data" );
assert.eq( 1 , c.findOne().a , "mongodump then restore has no broken data" );

resetDbpath( externalPath );

assert.eq( -1 , fileSize() , "mongoexport prep invalid" );
runMongoProgram( "mongoexport", "--host", "127.0.0.1:" + port, "-d", baseName, "-c", baseName, "--out", externalFile );
assert.lt( 10 , fileSize() , "file size changed" );

c.drop();
runMongoProgram( "mongoimport", "--host", "127.0.0.1:" + port, "-d", baseName, "-c", baseName, "--file", externalFile );
assert.soon( "c.findOne()" , "mongo import json A" );
assert( c.findOne() && 1 == c.findOne().a , "mongo import json B" );
