
t = db.regex9;
t.drop();

t.insert( { _id : 1 , a : [ "a" , "b" , "c" ] } )
t.insert( { _id : 2 , a : [ "a" , "b" , "c" , "d" ] } )
t.insert( { _id : 3 , a : [ "b" , "c" , "d" ] } )

assert.eq( 2 , t.find( { a : /a/  } ).itcount() , "A1" )
assert.eq( 2 , t.find( { a : { $regex : "a" }  } ).itcount() , "A2" )
assert.eq( 2 , t.find( { a : { $regex : /a/ }  } ).itcount() , "A3" )

// SERVER 4928
// assert when pattern is not string or regex
asserted=false
try {
    t.find( {a: { $regex: [/a/] } } ).itcount();
}
catch (e) {
    asserted=true
}
assert.eq( true, asserted, "SERVER4928");
