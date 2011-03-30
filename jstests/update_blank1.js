
t = db.update_blank1
t.drop();

orig = { _id : 1 , "" : 1 , "a" : 2 , "b" : 3 };
t.insert( orig );
assert.eq( orig , t.findOne() , "A1" );

t.update( {} , { $set : { "c" :  1 } } );
print( db.getLastError() );
orig["c"] = 1;
//assert.eq( orig , t.findOne() , "A2" ); // SERVER-2651
