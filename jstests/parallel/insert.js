// perform inserts in parallel from several clients

f = db.jstests_parallel_insert;
f.drop();
f.ensureIndex( {me:1} );

Random.setRandomSeed();

t = new ParallelTester();

test = function() {
    var args = argumentsToArray( arguments );
    var me = args.shift();
    var m = new Mongo( db.getMongo().host );
    var t = m.getDB( "test" ).jstests_parallel_insert;
    for( var i in args ) {
        sleep( args[ i ] );
        if ( i % 50 == 0 ) {
            assert.eq( i, t.count( { who:me } ) );
            print( me + " " + i );
        }
        t.save( { i:i, who:me } );
    }
}

for( i = 0; i < 10; ++i ) {
    var params = [ i ];
    var mean = Random.rand() * 20;
    for( j = 0; j < 1000; ++j ) {
        params.push( Random.genExp( mean ) );
    }
    t.add( test, params );
}

t.run( "one or more tests failed" );

assert( f.validate().valid );
