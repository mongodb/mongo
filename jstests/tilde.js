t = db.tilde;
t.drop();

t.insert( { } );
t.insert( { x : [1,2,3] } );
t.insert( { x : 99 } );
t.update( {x : 2}, { $inc : { "x.~" : 1 } } , false, true );
assert( t.findOne({x:1}).x[1] == 3, "A1" );

t.insert( { x : { y : [8,7,6] } } )

t.update( {'x.y' : 7}, { $inc : { "x.1" : 1 } } , false, true )
printjson( t.findOne({"x.y" : 8}) );
//assert.eq( 8 , t.findOne({"x.y" : 8}).x.y[1] , "B1" );

t.update( {'x.y' : 7}, { $inc : { "x.~" : 1 } } , false, true )
printjson( t.findOne({"x.y" : 8}) );
//assert.eq( 8 , t.findOne({"x.y" : 8}).x.y[1] , "B2" );
