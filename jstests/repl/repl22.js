
// test for SERVER-5040 - if documents move forward during an initial sync.

if ( 0 ) { // SERVER-5040
    
rt = new ReplTest( "repl22tests" );

master = rt.start( true );
md = master.getDB( 'd' );
mdc = md[ 'c' ];

for( i = 0; i < 1000000; ++i ) {
    mdc.insert( { _id:i, x:i } );
}
md.getLastError();

mdc.ensureIndex( { x : 1 }, { unique: true } );
md.getLastError();

slave = rt.start( false );
sc = slave.getDB( 'd' )[ 'c' ];

// Wait for slave to start cloning.
assert.soon( function() { c = sc.count(); /*print( c );*/ return c > 0; } );

// Move first document to the end by growing it
mdc.remove( { _id:1, x:1 } );
mdc.insert( { _id:1000001, x:1, bigstring: "ayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayayay" } );
md.getLastError();

mdc.insert( { _id:'sentinel' } );
md.getLastError();

// Wait for replication to catch up.
assert.soon( function() { return sc.count( { _id:'sentinel' } ) > 0; } );

// Do we have an index?
assert.eq (1, slave.getDB( 'd' )['system.indexes']
           .find({"v" : 1,"key" : {"x" : 1},"unique" : true,"ns" : "d.c","name" : "x_1"}).count());
}
