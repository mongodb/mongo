// Start mongod and run jstests/_runner.js

db = startMongod( "--port", "27018", "--dbpath", MongoRunner.dataDir + "/jstests" ).getDB( "test" );
load( "jstests/_runner.js" );
