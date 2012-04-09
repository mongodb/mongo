
load( "jstests/libs/slow_weekly_util.js" )
testServer = new SlowWeeklyMongod( "query_yield2" )
db = testServer.getDB( "test" );

t = db.query_yield2;
t.drop()

N = 200;
i = 0;

q = function(){ var x=this.n; for ( var i=0; i<25000; i++ ){ x = x * 2; } return false; }

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

join = startParallelShell( "print( 0 == db.query_yield2.find( function(){ var x=this.n; for ( var i=0; i<50000; i++ ){ x = x * 2; } return false; } ).itcount() ); " )

assert.soon( 
    function(){
        var x = db.currentOp().inprog;
        return x.length > 0;
    } , "never doing query" , 2000 , 1 
);

print( "start query" );

num = 0;
start = new Date();
while ( ( (new Date()).getTime() - start ) < ( time * 2 ) ){
    var me = Date.timeFunc( function(){ t.insert( { x : 1 } ); db.getLastError(); } )
    var x = db.currentOp()

    if ( num++ == 0 ){
        assert.eq( 1 , x.inprog.length , "nothing in prog" );
    }
    
    assert.gt( 200 , me );

    if ( x.inprog.length == 0 )
        break;

}

join();

var x = db.currentOp()
assert.eq( 0 , x.inprog.length , "weird 2" );

testServer.stop();
