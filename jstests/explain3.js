/** SERVER-2451 Kill cursor while explain is yielding */

// disable test until SERVER-2451 fixed
if ( false ) {

t = db.jstests_explain3;
t.drop();

t.ensureIndex( {i:1} );
for( var i = 0; i < 10000; ++i ) {
    t.save( {i:i,j:0} );
}
db.getLastError();

s = startParallelShell( "sleep( 20 ); db.jstests_explain3.dropIndex( {i:1} );" );

printjson( t.find( {i:{$gt:-1},j:1} ).hint( {i:1} ).explain() );

s();

}