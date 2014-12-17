port = 30201;

mongo = startMongodEmpty("--port", port,
                         "--dbpath", MongoRunner.dataPath + this.name,
                         "--smallfiles",
                         "--storageEngine", "devnull" );

db = mongo.getDB( "test" );

res = db.foo.insert( { x : 1 } );
assert.eq( 1, res.nInserted, tojson( res ) );

stopMongod( port );
