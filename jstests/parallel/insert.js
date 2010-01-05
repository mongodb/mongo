// perform inserts in parallel from several clients

f = db.jstests_parallel_insert;
f.drop();
f.ensureIndex( {me:1} );

seed = new Date().getTime();
print( "random seed: " + seed );
srand( seed );

expTimeout = function( mean ) {
    return -Math.log( rand() ) * mean;
}

test = function() {
    args = argumentsToArray( arguments );
    var me = args.shift();
    var m = new Mongo( db.getMongo().host );
    var t = m.getDB( "test" ).jstests_parallel_insert;
    for( var i in args ) {
        sleep( args[ i ] );
        if ( i % 50 == 0 ) {
            assert.eq( i, t.count( { who:me } ) );
//            print( me + " " + i );
        }
        t.save( { i:i, who:me } );
    }
}

argvs = new Array();
for( i = 0; i < 10; ++i ) {
    argvs.push( [ i ] );
    var mean = rand() * 20;
    for( j = 0; j < 1000; ++j ) {
        argvs[ argvs.length - 1 ].push( expTimeout( mean ) );
    }
}

assert.parallelTests( test, argvs, "one or more tests failed" );

assert( f.validate().valid );
