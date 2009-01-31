t = db.jni8;
t.drop();

t.save( { a : 1 , b : [ 2 , 3 , 4 ] } );

assert.eq( 1 , t.find().length() );
assert.eq( 1 , t.find( function(){ return this.a == 1; } ).length() );
assert.eq( 1 , t.find( function(){ return this.b[0] == 2; } ).length() );
assert.eq( 0 , t.find( function(){ return this.b[0] == 3; } ).length() );
assert.eq( 1 , t.find( function(){ return this.b[1] == 3; } ).length() );

assert(t.validate().valid);
