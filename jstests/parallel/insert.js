// perform inserts in parallel from several clients

f = db.jstests_parallel_insert;
f.drop();
f.ensureIndex( {who:1} );

Random.setRandomSeed();

t = new ParallelTester();

for( id = 0; id < 10; ++id ) {
    var g = new EventGenerator( id, "jstests_parallel_insert", Random.randInt( 20 ) );
    for( j = 0; j < 1000; ++j ) {
        if ( j % 50 == 0 ) {
            g.addCheckCount( j, {who:id} );
        }
        g.addInsert( { i:j, who:id } );
    }
    t.add( EventGenerator.dispatch, g.getEvents() );
}

t.run( "one or more tests failed" );

assert( f.validate().valid );
