t = db.jstests_mr_killop;
t.drop();
t2 = db.jstests_mr_killop_out;
t2.drop();

if ( typeof _threadInject == "undefined" ) { // don't run in v8 mode - SERVER-1900 

    function debug( x ) {
//        printjson( x );
    }

    /** @return op code for map reduce op created by spawned shell, or that op's child */
    function op( where ) {
        p = db.currentOp().inprog;
        debug( p );
        for ( var i in p ) {
            var o = p[ i ];
            if ( where ) {
                if ( o.active && o.ns == "test.jstests_mr_killop" && o.query && o.query.$where ) {
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
        t.drop();
        t2.drop();
        // Ensure we have 2 documents for the reduce to run
        t.save( {a:1} );
        t.save( {a:1} );
        db.getLastError();
                
        spec =
            {
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
        assert.soon( function() { o = op( where ); return o != -1 } );

        res = db.killOp( o );
        debug( "did kill : " + tojson( res ) );
        
        // When the map reduce op is killed, the spawned shell will exit
        s();
        debug( "parallel shell completed" );
        
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
        test( function() { emit( this.a, 1 ); }, loop, null, null, where );
        test( function() { loop(); }, function( k, v ) { return v[ 0 ] }, null, { loop: loop }, where );
    }

    /** Test looping in finalize function */
    function runFinalizeTests( loop, where ) {
        test( function() { emit( this.a, 1 ); }, function( k, v ) { return v[ 0 ] }, loop, null, where );
        test( function() { emit( this.a, 1 ); }, function( k, v ) { return v[ 0 ] }, function( a, b ) { loop() }, { loop: loop }, where );
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
        db.jstests_mr_killop.find( {$where:function() { while( 1 ) { ; } }} ).toArray();
    }
    runMRTests( loop, true );

}
