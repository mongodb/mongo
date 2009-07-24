
t = db.basica;


t.drop();

t.save( { a : 1 , b :  [ { x : 2 , y : 2 } , { x : 3 , y : 3 } ]  } );

x = t.findOne();
x.b["0"].x = 4;
x.b["0"].z = 4;
x.b[0].m = 9;
x.b[0]["asd"] = 11;
x.a = 2;
x.z = 11;

tojson( x );
t.save( x );
assert.eq( tojson( x ) , tojson( t.findOne() ) , "FIRST" );

// -----

t.drop();

t.save( { a : 1 , b :  [ { x : 2 , y : 2 } , { x : 3 , y : 3 } ]  } );

x = t.findOne();
x.b["0"].z = 4;

//printjson( x );
t.save( x );
assert.eq( tojson( x ) , tojson( t.findOne() ) , "SECOND" );

