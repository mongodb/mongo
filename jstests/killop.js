t = db.jstests_killop
t.drop();

function debug( x ) {
    printjson( x );
}

t.save( {} );
db.getLastError();

function ops() {
    p = db.currentOp().inprog;
    debug( p );
    ids = [];
    for ( var i in p ) {
        var o = p[ i ];
        if ( o.active && o.query && o.query.query && o.query.query.$where && o.ns == "test.jstests_killop" ) {
            ids.push( o.opid );
        }
    }
    return ids;
}

s1 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );
s2 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );

o = [];
assert.soon( function() { o = ops(); return o.length == 2; } );
db.killOp( o[ 0 ] );
db.killOp( o[ 1 ] );

start = new Date();

s1();
s2();

// don't want to pass if timeout killed the js function
assert( ( new Date() ) - start < 30000 );
