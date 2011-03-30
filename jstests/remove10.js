// SERVER-2009 Update documents with adjacent indexed keys.
// This test doesn't fail, it just prints an invalid warning message.

if ( 0 ) { // SERVER-2009
t = db.jstests_remove10;
t.drop();
t.ensureIndex( {i:1} );

function arr( i ) {
    ret = [];
 	for( j = i; j < i + 11; ++j ) {
        ret.push( j );
    }
    return ret;
}

for( i = 0; i < 1100; i += 11 ) {
    t.save( {i:arr( i )} );
}

s = startParallelShell( 't = db.jstests_remove10; for( j = 0; j < 1000; ++j ) { o = t.findOne( {i:Random.randInt(1100)} ); t.remove( {_id:o._id} ); t.insert( o ); }' );

for( i = 0; i < 200; ++i ) {
    t.find( {i:{$gte:0}} ).hint( {i:1} ).itcount();
}

s();
}