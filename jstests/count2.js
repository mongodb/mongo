t = db.count2;
t.drop();

for ( var i=0; i<1000; i++ ){
    t.save( { num : i , m : i % 20 } );
}

assert.eq( 1000 , t.count() , "A" )
assert.eq( 1000 , t.find().count() , "B" )
assert.eq( 1000 , t.find().toArray().length , "C" )

assert.eq( 50 , t.find( { m : 5 } ).toArray().length , "D" )
assert.eq( 50 , t.find( { m : 5 } ).count() , "E" )

assert.eq( 40 , t.find( { m : 5 } ).skip( 10 ).toArray().length , "F" )
assert.eq( 50 , t.find( { m : 5 } ).skip( 10 ).count() , "G" )
assert.eq( 40 , t.find( { m : 5 } ).skip( 10 ).countReturn() , "H" )

assert.eq( 20 , t.find( { m : 5 } ).skip( 10 ).limit(20).toArray().length , "I" )
assert.eq( 50 , t.find( { m : 5 } ).skip( 10 ).limit(20).count() , "J" )
assert.eq( 20 , t.find( { m : 5 } ).skip( 10 ).limit(20).countReturn() , "K" )

assert.eq( 5 , t.find( { m : 5 } ).skip( 45 ).limit(20).countReturn() , "L" )
