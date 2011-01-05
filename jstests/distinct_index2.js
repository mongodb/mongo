t = db.distinct_index2;
t.drop();

t.ensureIndex( { a : 1 , b : 1 } )
t.ensureIndex( { c : 1 } )

function x(){
    return Math.floor( Math.random() * 10 );
}

for ( i=0; i<2000; i++ ){
    t.insert( { a : x() , b : x() , c : x() } )
}

correct = []
for ( i=0; i<10; i++ )
    correct.push( i )

function check( field ){
    res = t.distinct( field )
    res = res.sort()
    assert.eq( correct , res , "check: " + field );

    if ( field != "a" ){
        res = t.distinct( field , { a : 1 } )
        res = res.sort()
        assert.eq( correct , res , "check 2: " + field );
    }
}

check( "a" )
check( "b" )
check( "c" )


