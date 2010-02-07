
t = db.stats1;
t.drop();

t.save( { a : 1 } );

assert.lt( 0 , t.dataSize() , "A" );
assert.lt( t.dataSize() , t.storageSize() , "B" );
assert.lt( 0 , t.totalIndexSize() , "C" );
