// The 'cursor not found in map -1' warning is not logged when get more exhausts a client cursor.
// SERVER-6931

t = db.jstests_cursorb;
t.drop();

// Exhaust a client cursor in get more.
for( i = 0; i < 200; ++i ) {
    t.save( { a:i } );
}
t.find().itcount();

// Check that the 'cursor not found in map -1' message is not printed.  This message indicates an
// attempt to look up a cursor with an invalid id and should never appear in the log.
log = db.adminCommand( { getLog:'global' } ).log
log.forEach( function( line ) { assert( !line.match( /cursor not found in map -1 / ),
                                        'Cursor map lookup with id -1.' ); } );
