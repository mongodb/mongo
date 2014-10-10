
t = db.basic8;
t.drop();

t.save( { a : 1 } );
o = t.findOne();
o.b = 2;
t.save( o );

assert.eq( 1 , t.find().count() , "A" );
assert.eq( 2 , t.findOne().b , "B" );
