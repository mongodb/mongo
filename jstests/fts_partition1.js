load( "jstests/libs/fts.js" )

t = db.text_parition1;
t.drop();

t.insert( { _id : 1 , x : 1 , y : "foo" } );
t.insert( { _id : 2 , x : 1 , y : "bar" } );
t.insert( { _id : 3 , x : 2 , y : "foo" } );
t.insert( { _id : 4 , x : 2 , y : "bar" } );

t.ensureIndex( { x : 1, y : "text" } );

res = t.runCommand( "text", { search : "foo" } );
assert.eq( 0, res.ok, tojson(res) );

assert.eq( [ 1 ], queryIDS( t, "foo" , { x : 1 } ) );

res = t.runCommand( "text", { search : "foo" , filter : { x : 1 } } );
assert( res.results[0].score > 0, tojson( res ) )

// repeat search with "language" specified, SERVER-8999
res = t.runCommand( "text", { search : "foo" , filter : { x : 1 } , language : "english" } );
assert( res.results[0].score > 0, tojson( res ) )
