load( "jstests/libs/slow_weekly_util.js" )
testServer = new SlowWeeklyMongod( "query_yield1" )
db = testServer.getDB( "test" );

t = db.query_yield1;
t.drop()

N = 20000;
i = 0;

q = function(){ var x=this.n; for ( var i=0; i<250; i++ ){ x = x * 2; } return false; }

while ( true ){
    function fill(){
        for ( ; i<N; i++ ){
            t.insert( { _id : i , n : 1 } )
        }
    }
    
     function timeQuery(){
        return Date.timeFunc( 
            function(){
                assert.eq( 0 , t.find( q ).itcount() );
            }
        );
        
    }
    
    fill();
    timeQuery();
    timeQuery();
    time = timeQuery();
    print( N + "\t" + time );
    if ( time > 2000 )
        break;
    
    N *= 2;
}

// --- test 1

assert.eq( 0, db.currentOp().inprog.length , "setup broken" );

join = startParallelShell( "print( 0 == db.query_yield1.find( function(){ var x=this.n; for ( var i=0; i<500; i++ ){ x = x * 2; } return false; } ).itcount() ); " )

assert.soon( 
    function(){
        var x = db.currentOp().inprog;
        return x.length > 0;
    } , "never doing query" , 2000 , 1 
);

print( "start query" );

num = 0;
start = new Date();
biggestMe = 0;
while ( ( (new Date()).getTime() - start ) < ( time * 2 ) ){
    var me = Date.timeFunc( function(){ t.insert( { x : 1 } ); db.getLastError(); } )
    var x = db.currentOp()

    if ( num++ == 0 ){
        assert.eq( 1 , x.inprog.length , "nothing in prog" );
    }
    
    if ( me > biggestMe ) {
        biggestMe = me;
        print( "biggestMe: " + biggestMe );
    }

    assert.gt( 200 , me , "took too long for me to run" );

    if ( x.inprog.length == 0 )
        break;

}

join();

var x = db.currentOp()
assert.eq( 0 , x.inprog.length , "weird 2" );

testServer.stop();

