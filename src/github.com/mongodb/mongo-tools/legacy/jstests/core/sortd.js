// Test sorting with dups and multiple candidate query plans.

t = db.jstests_sortd;

function checkNumSorted( n, query ) {
    docs = query.toArray();
    assert.eq( n, docs.length );
    for( i = 1; i < docs.length; ++i ) {
        assert.lte( docs[ i-1 ].a, docs[ i ].a );
    }
}


// Test results added by ordered and unordered plans, unordered plan finishes.

t.drop();

t.save( {a:[1,2,3,4,5]} );
t.save( {a:10} );
t.ensureIndex( {a:1} );

assert.eq( 2, t.find( {a:{$gt:0}} ).sort( {a:1} ).itcount() );
assert.eq( 2, t.find( {a:{$gt:0},b:null} ).sort( {a:1} ).itcount() );

// Test results added by ordered and unordered plans, ordered plan finishes.

t.drop();

t.save( {a:1} );
t.save( {a:10} );
for( i = 2; i <= 9; ++i ) {
    t.save( {a:i} );
}
for( i = 0; i < 30; ++i ) {
    t.save( {a:100} );
}
t.ensureIndex( {a:1} );

checkNumSorted( 10, t.find( {a:{$gte:0,$lte:10}} ).sort( {a:1} ) );
checkNumSorted( 10, t.find( {a:{$gte:0,$lte:10},b:null} ).sort( {a:1} ) );

// Test results added by ordered and unordered plans, ordered plan finishes and continues with getmore.

t.drop();

t.save( {a:1} );
t.save( {a:200} );
for( i = 2; i <= 199; ++i ) {
    t.save( {a:i} );
}
for( i = 0; i < 30; ++i ) {
    t.save( {a:2000} );
}
t.ensureIndex( {a:1} );

checkNumSorted( 200, t.find( {a:{$gte:0,$lte:200}} ).sort( {a:1} ) );
checkNumSorted( 200, t.find( {a:{$gte:0,$lte:200},b:null} ).sort( {a:1} ) );

// Test results added by ordered and unordered plans, with unordered results excluded during
// getmore.

t.drop();

for( i = 399; i >= 0; --i ) {
    t.save( {a:i} );
}
t.ensureIndex( {a:1} );

checkNumSorted( 400, t.find( {a:{$gte:0,$lte:400},b:null} ).batchSize( 50 ).sort( {a:1} ) );

