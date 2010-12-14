t = db.jstests_mr_killop;
t.drop();

if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode - SERVER-1900

t.save( {a:1} );
db.getLastError();

function debug( x ) {
//    printjson( x );
}

/** @return op code for map reduce op created by spawned shell, or that op's child */
function op( where ) {
    p = db.currentOp().inprog;
    debug( p );
    for ( var i in p ) {
        var o = p[ i ];
        if ( where ) {
            if ( o.active && o.ns == "test.jstests_mr_killop" && o.query && o.query.query && o.query.query.$where ) {
                return o.opid;
            }
        } else {
            if ( o.active && o.query && o.query.mapreduce && o.query.mapreduce == "jstests_mr_killop" ) {
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
 * @where - if true, a count $where op is killed rather than the map reduce op.
 * This is necessay for a child count $where of a map reduce op because child
 * ops currently mask parent ops in currentOp.
 */
function testOne( map, reduce, finalize, scope, where, wait ) {
    spec =
    {
    mapreduce:"jstests_mr_killop",
    map: map,
    reduce: reduce
    };
    if ( finalize ) {
        spec[ "finalize" ] = finalize;
    }
    if ( scope ) {
        spec[ "scope" ] = scope;
    }

    s = startParallelShell( "db.runCommand( " + tojson( spec ) + " );" );
    
    if ( wait ) {
        sleep( 2000 );
    }
    
    o = null;
    assert.soon( function() { o = op( where ); return o != -1 } );

    db.killOp( o );
    
    // When the map reduce op is killed, the spawned shell will exit
    s();
    
    assert.eq( -1, op( where ) );
}

/** Test using wait and non wait modes */
function test( map, reduce, finalize, scope, where ) {
    testOne( map, reduce, finalize, scope, where, false );
    testOne( map, reduce, finalize, scope, where, true );
}

/** Test looping in map and reduce functions */
function runMRTests( loop, where ) {
    test( loop, function( k, v ) { return v[ 0 ]; }, null, null, where );
    test( function() { emit( this.id, 1 ); }, loop, null, null, where );
    test( function() { loop(); }, function( k, v ) { return v[ 0 ] }, null, { loop: loop }, where );
}

/** Test looping in finalize function */
function runFinalizeTests( loop, where ) {
    test( function() { emit( this.id, 1 ); }, function( k, v ) { return v[ 0 ] }, loop, null, where );
    test( function() { emit( this.id, 1 ); }, function( k, v ) { return v[ 0 ] }, function( a, b ) { loop() }, { loop: loop }, where );
}

var loop = function() {
    while( 1 ) {
        ;
    }
}
runMRTests( loop, false );
runFinalizeTests( loop, false );

var loop = function() {
    while( 1 ) {
        db.jstests_mr_killop.count( { a:1 } );
    }
}
runMRTests( loop, false );
// db can't be accessed from finalize() so not running that test

/** Test that we can kill the child op of a map reduce op */
var loop = function() {
    db.jstests_mr_killop.count( {$where:function() { while( 1 ) { ; } }} );
}
runMRTests( loop, true );

}