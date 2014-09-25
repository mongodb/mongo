
t = db.eval8;
t.drop();

x = { a : 1 , b : 2 };
t.save( x );
x = t.findOne();

assert( x.a && x.b , "A" );
delete x.b;

assert( x.a && ! x.b , "B" )
x.b = 3;
assert( x.a && x.b , "C" );
assert.eq( 3 , x.b , "D" );

t.save( x );
y = t.findOne();
assert.eq( tojson( x ) , tojson( y ) , "E" );
