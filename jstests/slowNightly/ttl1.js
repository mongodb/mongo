/**
 * Part 1: Simple test of TTL.  Create a new collection with 24 docs, with timestamps at one hour
 *         intervals, from now-minus-23 hours ago until now.  Also add some docs with non-date
 *         values.  Then create a TTL index that expires all docs older than ~5.5 hours (20000
 *         seconds).  Wait 70 seconds (TTL monitor runs every 60) and check that 18 docs deleted.
 * Part 2: Add a second TTL index on an identical field. The second index expires docs older than
 *         ~2.8 hours (10000 seconds). Wait 70 seconds and check that 3 more docs deleted.
 */

// Part 1
var t = db.ttl1;
t.drop();

var now = (new Date()).getTime();

for ( i=0; i<24; i++ ){
    var past = new Date( now - ( 3600 * 1000 * i ) );
    t.insert( { x : past , y : past } );
}
t.insert( { a : 1 } )     //no x value
t.insert( { x: null } )   //non-date value
t.insert( { x : true } )  //non-date value
t.insert( { x : "yo" } )  //non-date value
t.insert( { x : 3 } )     //non-date value
t.insert( { x : /foo/ } ) //non-date value
db.getLastError();

assert.eq( 30 , t.count() );

t.ensureIndex( { x : 1 } , { expireAfterSeconds : 20000 } );

assert.soon( 
    function() {
        return t.count() < 30;
    }, "TTL index on x didn't delete" , 70 * 1000
);

assert.eq( 0 , t.find( { x : { $lt : new Date( now - 20000000 ) } } ).count() );
assert.eq( 12 , t.count() );

assert.lte( 18, db.serverStatus().metrics.ttl.deletedDocuments );
assert.lte( 1, db.serverStatus().metrics.ttl.passes );

// Part 2
t.ensureIndex( { y : 1 } , { expireAfterSeconds : 10000 } );

assert.soon(
    function() {
        return t.count() < 12;
    }, "TTL index on y didn't delete" , 70 * 1000
);

assert.eq( 0 , t.find( { y : { $lt : new Date( now - 10000000 ) } } ).count() );
assert.eq( 9 , t.count() );
