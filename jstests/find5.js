
t = db.find5;
t.drop();

t.save({a: 1});
t.save({b: 5});

assert.eq(1, t.find({}, {b:1}).count(), "A");

o = t.find( {} , {b:1} )[0];

assert.eq(5, o.b, "B");
assert(!o.a, "C");

t.drop();
t.save( { a : 1 , b : { c : 2 , d : 3 } } );
assert.eq( 2 , t.find( {} , { "b.c" : 1 } ).toArray()[0].b.c , "D" );
