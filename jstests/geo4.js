var t = db.geo4;
t.drop();

t.insert( { zip : "06525" , loc : [ 41.352964 , 73.01212  ] } );

t.ensureIndex( { loc : "2d" }, { bits : 33 } );
assert.eq( db.getLastError() , "bits in geo index must be between 1 and 32" , "a" );

t.ensureIndex( { loc : "2d" }, { bits : 32 } );
assert( !db.getLastError(), "b" );
