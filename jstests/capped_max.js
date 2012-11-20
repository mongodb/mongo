
t = db.capped_max;
sz = 1024 * 16;

t.drop();
db.createCollection( t.getName() , {capped: true, size: sz } );
assert.eq( Math.pow(2,63), t.stats().max );

t.drop();
db.createCollection( t.getName() , {capped: true, size: sz, max: 123456 } );
assert.eq( 123456, t.stats().max );

t.drop();
res = db.createCollection( t.getName() , {capped: true, size: sz, max: Math.pow(2, 31) } );
assert.eq( 0, res.ok, tojson(res) );
assert.eq( 0, t.stats().ok )

