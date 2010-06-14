skipIfTestingReplication();

t = db.jstests_indexh;

// index extent freeing
t.drop();
t.save( {} );
var s1 = db.stats().dataSize;
t.ensureIndex( {a:1} );
var s2 = db.stats().dataSize;
assert.automsg( "s1 < s2" );
t.dropIndex( {a:1} );
assert.eq.automsg( "s1", "db.stats().dataSize" );

// index node freeing
t.drop();
t.ensureIndex( {a:1} );
var big = new Array( 1000 ).toString();
for( i = 0; i < 1000; ++i ) {
    t.save( {a:i,b:big} );
}
var s3 = db.stats().indexSize;
t.remove( {} );
assert.automsg( "db.stats().indexSize < s3" );