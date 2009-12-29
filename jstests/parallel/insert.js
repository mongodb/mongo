// perform inserts in parallel from several clients

f = db.jstests_parallel_basic;
f.drop();
f.ensureIndex( {me:1} );

// would be nice to configure and log a random seed so we can reproduce
// behavior.  Seems like that's impossible using Math.random(), though :(

expTimeout = function( mean ) {
    return -Math.log( Math.random() ) * mean;
}

test = function( mean, me ) {
    var m = new Mongo( db.getMongo().host );
    var t = m.getDB( "test" ).jstests_parallel_basic;
    for( var i = 0; i < 1000; ++i ) {
        sleep( expTimeout( mean ) );
        if ( i % 50 == 0 ) {
            assert.eq( i, t.count( { who:me } ) );
            print( me + " " + i );
        }
        t.save( { i:i, who:me } );
    }
}

argvs = Array();
for( i = 0; i < 10; ++i ) {
    argvs.push( [ Math.random() * 20, i ] );
}

assert.parallelTests( test, argvs, "one or more tests failed" );

assert( f.validate().valid );
