
t = db.index_fornew;
t.drop();

t.insert( { x : 1 } )
t.ensureIndex( { x : 1 } , { v : 1 } )
assert.eq( 1 , t.getIndexes()[1].v , tojson( t.getIndexes() ) );

assert.throws( function(){ t.findOne( { x : 1 } ); } )

t.reIndex();
assert.eq( 0 , t.getIndexes()[1].v , tojson( t.getIndexes() ) );
assert( t.findOne( { x : 1 } ) );
