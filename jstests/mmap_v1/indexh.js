// This should get skipped when testing replication

t = db.jstests_indexh;

function debug( t ) {
    print( t );
}

function extraDebug() {
//    printjson( db.stats() );
//    db.printCollectionStats();    
}

// index extent freeing
t.drop();
t.save( {} );
var s1 = db.stats().dataSize;
debug( "s1: " + s1 );
extraDebug();
t.ensureIndex( {a:1} );
var s2 = db.stats().dataSize;
debug( "s2: " + s2 );
assert.automsg( "s1 < s2" );
t.dropIndex( {a:1} );
var s3 = db.stats().dataSize;
debug( "s3: " + s3 );
extraDebug();
assert.eq.automsg( "s1", "s3" );

// index node freeing
t.drop();
t.ensureIndex( {a:1} );
for( i = 'a'; i.length < 500; i += 'a' ) {
    t.save( {a:i} );
}
var s4 = db.stats().indexSize;
debug( "s4: " + s4 );
t.remove( {} );
var s5 = db.stats().indexSize;
debug( "s5: " + s5 );
assert.automsg( "s5 < s4" );