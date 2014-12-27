// test max docs in capped collection

t = db.capped_max1;
t.drop();

max = 10;
maxSize = 64 * 1024;
db.createCollection( t.getName() , {capped: true, size: maxSize, max: max } );
assert.eq( max, t.stats().max );
assert.eq( maxSize, t.stats().maxSize );
assert.eq( Math.floor(maxSize/1000), t.stats(1000).maxSize );

for ( var i=0; i < max * 2; i++ ) {
    t.insert( { x : 1 } );
}

assert.eq( max, t.count() );
