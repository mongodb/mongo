// Drop a collection while a map/reduce job is running against it.  SERVER-6757

t = db.jstests_mr_drop;
t.drop();

Random.setRandomSeed();

// Set sleep times for different stages of the map/reduce job.  The collection drop will occur
// during different stages of map/reduce depending on these sleep values.
mapSleep = Random.randInt( 4 );
reduceSleep = Random.randInt( 4 );
finalizeSleep = Random.randInt( 4 );

// Insert some documents.
for( i = 0; i < 10000; ++i ) {
    t.save( { key:parseInt( i / 2 ),
              mapSleep:mapSleep,
              reduceSleep:reduceSleep,
              finalizeSleep:finalizeSleep } );
}
db.getLastError();

// Schedule a collection drop two seconds in the future.
s = startParallelShell( "sleep( 2000 ); db.jstests_mr_drop.drop();" );

// Run the map/reduce job.  Check for command failure internally.  The job succeeds even if the
// source collection is dropped in progress.
t.mapReduce( function() { sleep( this.mapSleep ); emit( this.key, this ); },
             function( key, vals ) { sleep( vals[ 0 ].reduceSleep ); return vals[ 0 ]; },
             { finalize:function( key, value ) { sleep( value.finalizeSleep ); return value; },
               out:'jstests_mr_drop_out' }
           );

// Wait for the parallel shell to finish.
s();

// Ensure the server is still alive.  Under SERVER-6757 the server can crash.
assert( !db.getLastError() );
