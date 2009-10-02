
t = db.basicb;
t.drop();

assert.throws( "t.insert( { '$a' : 5 } );" );
t.insert( { '$a' : 5 } , true );

