// perform inserts in parallel from a large number of clients

f = db.jstests_parallel_manyclients;
f.drop();
f.ensureIndex( {who:1} );

Random.setRandomSeed();

t = new ParallelTester();

for( id = 0; id < 200; ++id ) {
    var g = new EventGenerator( id, "jstests_parallel_manyclients", Random.randInt( 20 ) );
    for( j = 0; j < 1000; ++j ) {
        if ( j % 50 == 0 ) {
            g.addCheckCount( j, {who:id}, true );
        }
        g.addInsert( { i:j, who:id } );
    }
    t.add( EventGenerator.dispatch, g.getEvents() );
}

print( "done preparing test" );

t.run( "one or more tests failed" );

assert( f.validate().valid );
