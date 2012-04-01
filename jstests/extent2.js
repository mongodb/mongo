

mydb = db.getSisterDB( "test_extent2" );
mydb.dropDatabase();

t = mydb.foo;
e = mydb["$freelist"]

function insert(){
    t.insert( { _id : 1 , x : 1 } )
    t.insert( { _id : 2 , x : 1 } )
    t.insert( { _id : 3 , x : 1 } )
    t.ensureIndex( { x : 1 } );
}

insert();
t.drop();

start = e.stats();

for ( i=0; i<100; i++ ) {
    insert();
    t.drop();
}

end = e.stats();

printjson( start );
printjson( end )
assert.eq( 4 , start.numExtents );
assert.eq( 4 , end.numExtents );
