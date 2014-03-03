
name = "server2639"

t = db.getCollection( name );
t.drop();


db.createCollection( name , { capped : true , size : 1 } );

size = t.stats().storageSize;

bigString = "";
while ( bigString.length < size )
    bigString += ".";

t.insert( { x : 1 } );

t.insert( { x : 2 , bigString : bigString } );
gle = db.getLastErrorObj();
assert.eq( 16328 , gle.code , tojson( gle ) )

assert.eq( 1 , t.count() ); // make sure small doc didn't get deleted
assert.eq( 1 , t.findOne().x );

// make sure can still insert
t.insert( { x : 2 } );
assert.eq( 2 , t.count() );
