// Start mongod and run jstests/_runner.js

db = startMongod( "--port", "27018", "--dbpath", "/data/db/jstests" ).getDB( "test" );
load( "jstests/_runner.js" );
