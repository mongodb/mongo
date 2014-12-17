// Test an in memory sort memory assertion after a plan has "taken over" in the query optimizer
// cursor.

t = db.jstests_sortj;
t.drop();

t.ensureIndex( { a:1 } );

big = new Array( 100000 ).toString();
for( i = 0; i < 1000; ++i ) {
    t.save( { a:1, b:big } );
}

assert.throws( function() {
              t.find( { a:{ $gte:0 }, c:null } ).sort( { d:1 } ).itcount();
              } );
t.drop();