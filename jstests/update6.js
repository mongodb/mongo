
t = db.update6;
t.drop();

t.save( { a : 1 , b : { c : 1 , d : 1 } } );

t.update( { a : 1 } , { $inc : { "b.c" : 1 } } );
assert.eq( 2 , t.findOne().b.c , "A" );
assert.eq( "c,d" , t.findOne().b.keySet().toString() , "B" );

t.update( { a : 1 } , { $inc : { "b.0e" : 1 } } );
assert.eq( 1 , t.findOne().b["0e"] , "C" );
assert.eq( "0e,c,d" , t.findOne().b.keySet().toString() , "D" );

// -----

t.drop();

t.save( {"_id" : 2 , 
         "b3" : {"0720" : 5 , "0721" : 12 , "0722" : 11 , "0723" : 3 , "0721" : 12} , 
         //"b323" : {"0720" : 1} , 
        }
      );


assert.eq( 4 , t.find({_id:2},{b3:1})[0].b3.keySet().length , "test 1 : ks before" );
t.update({_id:2},{$inc: { 'b3.0719' : 1}},true)
assert.eq( 5 , t.find({_id:2},{b3:1})[0].b3.keySet().length , "test 1 : ks after" );


// -----

t.drop();

t.save( {"_id" : 2 , 
         "b3" : {"0720" : 5 , "0721" : 12 , "0722" : 11 , "0723" : 3 , "0721" : 12} , 
         "b324" : {"0720" : 1} , 
        }
      );


assert.eq( 4 , t.find({_id:2},{b3:1})[0].b3.keySet().length , "test 2 : ks before" );
printjson( t.find({_id:2},{b3:1})[0].b3 )
t.update({_id:2},{$inc: { 'b3.0719' : 1}} )
printjson( t.find({_id:2},{b3:1})[0].b3 )
assert.eq( 5 , t.find({_id:2},{b3:1})[0].b3.keySet().length , "test 2 : ks after" );
