
t = db.update_multi6
t.drop();

t.update( { _id : 1 } , { _id : 1 , x : 1 , y : 2 } , true , false );
assert( t.findOne( { _id : 1 } ) , "A" )

t.update( { _id : 2 } , { _id : 2 , x : 1 , y : 2 } , true , true );
assert( db.getLastError() , "B: " + db.getLastErrorCmd() );

