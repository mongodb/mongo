
t = db.update_blank1
t.drop();

orig = { "" : 1 , _id : 2 , "a" : 3 , "b" : 4 };
t.insert( orig );
t.update( {} , { $set : { "c" :  5 } } );
print( db.getLastError() );
orig["c"] = 5;
assert.eq( orig , t.findOne() , "after $set" ); // SERVER-2651
