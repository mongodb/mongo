
t = db.not1;
t.drop();


t.insert({a:1})
t.insert({a:2})
t.insert({})

assert.eq( 3 , t.find().count() , "A" );
assert.eq( 1 , t.find( { a : 1 } ).count() , "B" );
//assert.eq( 2 , t.find( { a : { $ne : 1 } } ).count() , "C" );  // SERVER-198
assert.eq( 1 , t.find({a:{$in:[1]}}).count()  , "D" );
//assert.eq( 2 , t.find({a:{$nin:[1]}}).count() , "E" ); // SERVER-198
