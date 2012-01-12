// Yield and delete test case for query optimizer cursor.  SERVER-4401

t = db.jstests_distinct3;
t.drop();

t.ensureIndex({a:1});
t.ensureIndex({b:1});

for( i = 0; i < 50; ++i ) {
    for( j = 0; j < 20; ++j ) {
        t.save({a:i,c:i,d:j});
    }
}
for( i = 0; i < 1000; ++i ) {
    t.save({b:i,c:i+50});
}
db.getLastError();

// The idea here is to try and remove the last match for the {a:1} index scan while distinct is yielding.
p = startParallelShell( 'for( i = 0; i < 2500; ++i ) { db.jstests_distinct3.remove({a:49}); for( j = 0; j < 20; ++j ) { db.jstests_distinct3.save({a:49,c:49,d:j}) } }' );

for( i = 0; i < 100; ++i ) {
    count = t.distinct( 'c', {$or:[{a:{$gte:0},d:0},{b:{$gte:0}}]} ).length;
    assert.gt( count, 1000 );
}

p();
