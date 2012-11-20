
t = db.capped_max;
sz = 1024 * 16;

t.drop();
db.createCollection( t.getName() , {capped: true, size: sz } );
assert.lt( Math.pow( 2, 62 ), t.stats().max.floatApprox )

t.drop();
db.createCollection( t.getName() , {capped: true, size: sz, max: 123456 } );
assert.eq( 123456, t.stats().max );

t.drop();
mm = Math.pow(2, 31) - 1;
db.createCollection( t.getName() , {capped: true, size: sz, max: mm } );
assert.eq( mm, t.stats().max );

t.drop();
res = db.createCollection( t.getName() , {capped: true, size: sz, max: Math.pow(2, 31) } );
assert.eq( 0, res.ok, tojson(res) );
assert.eq( 0, t.stats().ok )

