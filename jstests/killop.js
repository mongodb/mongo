/**
 * Basic test of killop functionality.
 *
 * Theory of operation: Creates two operations that will take a long time, sends killop for those
 * operations, and then attempts to infer that the operations died because of killop, and not for
 * some other reason.
 *
 * NOTES:
 * The long operations are count({$where: function () { while (1) ; } }).  These operations do not
 * terminate until the server determines that they've spent too much time in JS execution, typically
 * after 30 seconds of wall clock time have passed.  For these operations to take a long time, the
 * counted collection must not be empty; hence an initial write to the collection is required.
 */
t = db.jstests_killop
t.drop();

t.save( {} );
db.getLastError();

/**
 * This function filters for the operations that we're looking for, based on their state and
 * the contents of their query object.
 */
function ops() {
    p = db.currentOp().inprog;
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

var s1 = null;
var s2 = null;
try {
    s1 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );
    s2 = startParallelShell( "db.jstests_killop.count( { $where: function() { while( 1 ) { ; } } } )" );

    o = [];
    assert.soon(function() { o = ops(); return o.length == 2; },
                { toString: function () { return tojson(db.currentOp().inprog); } },
               10000);
    db.killOp( o[ 0 ] );
    db.killOp( o[ 1 ] );
    start = new Date();
}
finally {
    if (s1) s1();
    if (s2) s2();
}

// don't want to pass if timeout killed the js function NOTE: This test will sometimes pass when the
// JS engine did actually kill the operation, because the JS timeout is 30 seconds of wall clock
// time from the moment the operation starts, but "start" measures from shortly after the test sends
// the killop message to the server.
assert( ( new Date() ) - start < 30000 );
