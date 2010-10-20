t = db.distinct_array1;
t.drop();

t.save( { a : [1,2,3] } )
t.save( { a : [2,3,4] } )
t.save( { a : [3,4,5] } )
t.save( { a : 9 } )


res = t.distinct( "a" );
assert.eq( "1,2,3,4,5,9" , res.toString() , "A1" );


//t.drop();

t.save( { a : [{b:"a"}, {b:"d"}] , c : 12 } );
t.save( { a : [{b:"b"}, {b:"d"}] , c : 12 } );
t.save( { a : [{b:"c"}, {b:"e"}] , c : 12 } );
t.save( { a : [{b:"c"}, {b:"f"}] , c : 12 } );
t.save( { a : [] , c : 12 } );
t.save( { a : { b : "z"} , c : 12 } );

res = t.distinct( "a.b" );
res.sort()
assert.eq( "a,b,c,d,e,f,z" , res.toString() , "B1" );
