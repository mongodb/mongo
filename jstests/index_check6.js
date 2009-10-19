
t = db.index_check6;
t.drop();

t.ensureIndex( { age : 1 , rating : 1 } );

for ( var age=10; age<50; age++ ){
    for ( var rating=0; rating<10; rating++ ){
        t.save( { age : age , rating : rating } );
    }
}

assert.eq( 10 , t.find( { age : 30 } ).explain().nscanned , "A" );
assert.eq( 20 , t.find( { age : { $gte : 29 , $lte : 30 } } ).explain().nscanned , "B" );

//assert.eq( 2 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : 5 } ).explain().nscanned , "C" ); // SERVER-371
//assert.eq( 4 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : { $gte : 4 , $lte : 5 } } ).explain().nscanned , "D" ); // SERVER-371
