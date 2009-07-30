
t = db.find6;
t.drop();

t.save( { a : 1 } )
t.save( { a : 1 , b : 1 } )

assert.eq( 2 , t.find().count() , "A" );
assert.eq( 1 , t.find( { b : null } ).count() , "B" );
assert.eq( 1 , t.find( "function() { return this.b == null; }" ).itcount() , "C" );
assert.eq( 1 , t.find( "function() { return this.b == null; }" ).count() , "D" );
