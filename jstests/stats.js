
t = db.stats1;
t.drop();

t.save( { a : 1 } );

assert.lt( 0 , t.dataSize().toNumber() , "A" );
assert.lt( t.dataSize() , t.storageSize().toNumber() , "B" );
assert.lt( 0 , t.totalIndexSize() , "C" );
