// sub1.js

t = db.sub1;
t.drop();

x = { a : 1 , b : { c : { d : 2 } } }

t.save( x );

y = t.findOne();

assert.eq( 1 , y.a );
assert.eq( 2 , y.b.c.d );
print( tojson( y ) );
