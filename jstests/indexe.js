
t = db.indexe;
t.drop();

num = 100000;

for ( i=0; i<num; i++){
    t.insert( { a : "b" } );
}

assert.eq( num , t.find().count() ,"A1" );
assert.eq( num , t.find( { a : "b" } ).count() , "B1" );
assert.eq( num , t.find( { a : "b" } ).itcount() , "C1" );

t.ensureIndex( { a : "b" } );

assert.eq( num , t.find().count() ,"A2" );
assert.eq( num , t.find( { a : "b" } ).count() , "B2" );
assert.eq( num , t.find( { a : "b" } ).itcount() , "C3" );
