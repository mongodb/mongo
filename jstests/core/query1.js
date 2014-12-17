
t = db.query1;
t.drop();

t.save( { num : 1 } );
t.save( { num : 3 } )
t.save( { num : 4 } );

num = 0;
total = 0;

t.find().forEach(
    function(z){
        num++;
        total += z.num;
    }
);

assert.eq( num , 3 , "num" )
assert.eq( total , 8 , "total" )

assert.eq( 3 , t.find()._addSpecial( "$comment" , "this is a test" ).itcount() , "B1" )
assert.eq( 3 , t.find()._addSpecial( "$comment" , "this is a test" ).count() , "B2" )

assert.eq( 3 , t.find( { "$comment" : "yo ho ho" } ).itcount() , "C1" )
assert.eq( 3 , t.find( { "$comment" : "this is a test" } ).count() , "C2" )
