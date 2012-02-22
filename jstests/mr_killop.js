// Test killop applied to m/r operations and child ops of m/r operations.

t = db.jstests_mr_killop;
t.drop();
t2 = db.jstests_mr_killop_out;
t2.drop();

function debug( x ) {
//        printjson( x );
}

/** @return op code for map reduce op created by spawned shell, or that op's child */
function op( childLoop ) {
    p = db.currentOp().inprog;
    debug( p );
    for ( var i in p ) {
        var o = p[ i ];
        // Identify a map/reduce or where distinct operation by its collection, whether or not
        // it is currently active.
        if ( childLoop ) {
            if ( ( o.active || o.waitingForLock ) &&
                o.query &&
                o.query.query &&
                o.query.query.$where &&
                o.query.distinct == "jstests_mr_killop" ) {
                return o.opid;
            }
        }
        else {
            if ( ( o.active || o.waitingForLock ) &&
                o.query &&
                o.query.mapreduce &&
                o.query.mapreduce == "jstests_mr_killop" ) {
                return o.opid;
            }
        }
    }
    return -1;
}

/**
* Run one map reduce with the specified parameters in a parallel shell, kill the
* map reduce op or its child op with killOp, and wait for the map reduce op to
* terminate.
* @param childLoop - if true, a distinct $where op is killed rather than the map reduce op.
* This is necessay for a child distinct $where of a map reduce op because child
* ops currently mask parent ops in currentOp.
*/
function testOne( map, reduce, finalize, scope, childLoop, wait ) {
    t.drop();
    t2.drop();
    // Ensure we have 2 documents for the reduce to run
    t.save( {a:1} );
    t.save( {a:1} );
    db.getLastError();
            
    spec = {
        mapreduce:"jstests_mr_killop",
        out:"jstests_mr_killop_out",
        map: map,
        reduce: reduce
    };
    if ( finalize ) {
        spec[ "finalize" ] = finalize;
    }
    if ( scope ) {
        spec[ "scope" ] = scope;
    }

    // Windows shell strips all double quotes from command line, so use
    // single quotes.
    stringifiedSpec = tojson( spec ).toString().replace( /\n/g, ' ' ).replace( /\"/g, "\'" );
    
    // The assert below won't be caught by this test script, but it will cause error messages
    // to be printed.
    s = startParallelShell( "assert.commandWorked( db.runCommand( " + stringifiedSpec + " ) );" );
    
    if ( wait ) {
        sleep( 2000 );
    }
    
    o = null;
    assert.soon( function() { o = op( childLoop ); return o != -1 } );

    res = db.killOp( o );
    debug( "did kill : " + tojson( res ) );
    
    // When the map reduce op is killed, the spawned shell will exit
    s();
    debug( "parallel shell completed" );
    
    assert.eq( -1, op( childLoop ) );
}

/** Test using wait and non wait modes */
function test( map, reduce, finalize, scope, childLoop ) {
    testOne( map, reduce, finalize, scope, childLoop, false );
    testOne( map, reduce, finalize, scope, childLoop, true );
}

/** Test looping in map and reduce functions */
function runMRTests( loop, childLoop ) {
    test( loop, function( k, v ) { return v[ 0 ]; }, null, null, childLoop );
    test( function() { emit( this.a, 1 ); }, loop, null, null, childLoop );
    test( function() { loop(); }, function( k, v ) { return v[ 0 ] },
         null, { loop: loop }, childLoop );
}

/** Test looping in finalize function */
function runFinalizeTests( loop, childLoop ) {
    test( function() { emit( this.a, 1 ); }, function( k, v ) { return v[ 0 ] },
         loop, null, childLoop );
    test( function() { emit( this.a, 1 ); }, function( k, v ) { return v[ 0 ] },
         function( a, b ) { loop() }, { loop: loop }, childLoop );
}

var loop = function() {
    while( 1 ) {
        ;
    }
}
runMRTests( loop, false );
runFinalizeTests( loop, false );

// The test will attempt to kill the mr operation making the above count call.  Sleep to
// try and allow the test to see the mr operation (as the count operation will obscure
// its parent mr operation).
var loop = function() {
    while( 1 ) {
        db.jstests_mr_killop.count( { a:1 } );
        sleep( 113 );
    }
}
runMRTests( loop, false );
// db can't be accessed from finalize() so not running that test

// Test that we can kill the child op of a map reduce op.  The distinct command is used because
// its currentOp() output includes namespace information, and it does not suffer from
// SERVER-2291 as count does.
var loop = function() {
    db.jstests_mr_killop.distinct( { a:1 }, {$where:function() { while( 1 ) { ; } }} );
}
runMRTests( loop, true );

