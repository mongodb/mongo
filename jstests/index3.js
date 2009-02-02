

t = db.index3;
t.dropIndexes();
t.remove( {} );
sleep( 100 );

assert( t.getIndexes().length() == 0 );

t.ensureIndex( { name : 1 } );
sleep( 100 );

t.save( { name : "a" } );

t.ensureIndex( { name : 1 } );
sleep( 1000 );

assert( t.getIndexes().length() == 1 );

assert(t.validate().valid);
