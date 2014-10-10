// test max docs in capped collection

t = db.capped_max1;
t.drop();

max = 10;

db.createCollection( t.getName() , {capped: true, size: 64 * 1024, max: max } );
assert.eq( max, t.stats().max );

for ( var i=0; i < max * 2; i++ ) {
    t.insert( { x : 1 } );
}

assert.eq( max, t.count() );

