
tn = "capped5"

t = db[tn]
t.drop();

db.createCollection( tn , {capped: true, size: 1024 * 1024 * 1 } );
t.insert( { _id : 5 , x : 11 , z : 52 } );

assert.eq( 0 , t.getIndexKeys().length , "A0" )
assert.eq( 52 , t.findOne( { x : 11 } ).z , "A1" );
assert.eq( 52 , t.findOne( { _id : 5 } ).z , "A2" );

t.ensureIndex( { _id : 1 } )
t.ensureIndex( { x : 1 } )

assert.eq( 52 , t.findOne( { x : 11 } ).z , "B1" );
assert.eq( 52 , t.findOne( { _id : 5 } ).z , "B2" );
