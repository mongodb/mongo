
load( "jstests/libs/fts.js" );

t = db.text1;
t.drop();

t.save( { _id : 1 , x : "az b c" } );
t.save( { _id : 2 , x : "az b" } );
t.save( { _id : 3 , x : "b c" } );
t.save( { _id : 4 , x : "b c d" } );

t.ensureIndex( { x : "text" } );

assert.eq( [1,2,3,4] , queryIDS( t , "c az" ) , "A1" );
assert.eq( [4] , queryIDS( t , "d" ) , "A2" );

idx = db.system.indexes.findOne( { ns: t.getFullName(), "weights.x" : 1 } )
assert( idx.v >= 1, tojson( idx ) )
assert( idx.textIndexVersion >= 1, tojson( idx ) )

