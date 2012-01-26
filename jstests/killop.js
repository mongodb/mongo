t = db.jstests_killop
t.drop();

//if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode - SERVER-1900

function debug( x ) {
    //printjson( x );
}

t.save( {} );
db.getLastError();

function ops() {
    p = db.currentOp().inprog;
    debug( p );
    ids = [];
    for ( var i in p ) {
        var o = p[ i ];
        // We *can't* check for ns, b/c it's not guaranteed to be there unless the query is active, which 
        // it may not be in our polling cycle - particularly b/c we sleep every second in both the query and
        // the assert
        if ( ( o.active || o.waitingForLock ) && o.query && o.query.query && o.query.query.$where && o.query.count == "jstests_killop" ) {
            ids.push( o.opid );
        }
    }
    return ids;
}

s1 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );
s2 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );

o = [];
assert.soon( function() { o = ops(); return o.length == 2; } );
debug( o );
db.killOp( o[ 0 ] );
db.killOp( o[ 1 ] );

start = new Date();

s1();
s2();

// don't want to pass if timeout killed the js function
assert( ( new Date() ) - start < 30000 );

//}
