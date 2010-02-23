var t = db.geo4;
t.drop();

t.insert( { zip : "06525" , loc : [ 41.352964 , 73.01212  ] } );

t.ensureIndex( { loc : "2d" }, { bits : 33 } );
assert.eq( db.getLastError() , "can't have more than 32 bits in geo index" , "a" );

t.ensureIndex( { loc : "2d" }, { bits : 32 } );
assert( !db.getLastError(), "b" );
