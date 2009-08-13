
t = db.indexd;
t.drop();

t.save( { a : 1 } );
t.ensureIndex( { a : 1 } );
db.indexd.$_id_.drop();
r = t.drop();
assert.eq( 1 , r.ok , "drop failed: " + tojson( r ) );


//db.indexd.$_id_.remove({});
