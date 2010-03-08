t = db.tilde;
t.drop();

print("Part 2");
t.insert( { } );
t.insert( { x : [1,2,3] } );
t.insert( { x : 99 } );
t.update( {x : 2}, { $inc : { "~" : 1 } } , false, true );
//assert( t.findOne({x:1}).x[1] == 3, "tilde broken?" );
printjson( t.findOne({x:1}) );

print("Part 2");

t.insert( { x : { y : [8,7,6] } } )
t.update( {'x.y' : 7}, { $inc : { "x.~" : 1 } } , false, true )
printjson( t.findOne({"x.y" : 8}) );
// assert( t.findOne({"x.y" : 8}).x.y[1] == 8, "tilde nested" );
