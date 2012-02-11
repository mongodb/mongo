// Test renaming a collection during a background index build.  SERVER-4820

if ( 0 ) { // SERVER-4820
c = db.jstests_index12;
c.drop();

s = startParallelShell (
' for( i = 0; i < 100; ++i ) {' +
'    db.getSisterDB( "admin" ).runCommand( {renameCollection:"test.jstests_index12", to:"test.jstests_index12b"} );' +
'    db.jstests_index12b.drop();' +
'    sleep( 30 );' +
' }'
);

for( i = 0; i < 10; ++i ) {
    for( j = 0; j < 1000; ++j ) {
        c.save( {a:j} );
    }
    c.ensureIndex( {i:1}, {background:true} );
    assert( !db.getLastError() );
}

s();
}