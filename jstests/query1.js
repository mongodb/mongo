
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
