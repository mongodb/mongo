load( "jstests/libs/slow_weekly_util.js" );
test = new SlowWeeklyMongod( "conc_update" );
db = test.getDB( "test" );
t = db.disk_reuse1;
t.drop();

N = 10000;

function k(){
    return Math.floor( Math.random() * N );
}

s = "";
while ( s.length < 1024 )
    s += "abc";

state = {}

for ( i=0; i<N; i++ )
    t.insert( { _id : i , s : s } );

orig = t.stats();

t.remove();

for ( i=0; i<N; i++ )
    t.insert( { _id : i , s : s } );

assert.eq( orig.storageSize , t.stats().storageSize , "A" )

for ( j=0; j<100; j++ ){
    for ( i=0; i<N; i++ ){
        var r = Math.random();
        if ( r > .5 )
            t.remove( { _id : i } )
        else
            t.insert( { _id : i , s : s } )
    }

    //printjson( t.stats() );    

    assert.eq( orig.storageSize , t.stats().storageSize , "B" + j  )
}


test.stop();
