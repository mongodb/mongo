// index skipping and previous index range negation

t = db.jstests_or9;
t.drop();

t.ensureIndex( {a:1,b:1} );

t.save( {a:2,b:2} );

function check( a, b, q ) {
    count = a;
    clauses = b;
    query = q;
    assert.eq.automsg( "count", "t.count( query )" );
    if ( clauses == 1 ) {
        assert.eq.automsg( "undefined", "t.find( query ).explain().clauses" );        
    } else {
        assert.eq.automsg( "clauses", "t.find( query ).explain().clauses.length" );
    }
}

check( 1, 1, { $or: [ { a: { $gte:1,$lte:3 } }, { a: 2 } ] } );
check( 1, 2, { $or: [ { a: { $gt:2,$lte:3 } }, { a: 2 } ] } );

check( 1, 1, { $or: [ { b: { $gte:1,$lte:3 } }, { b: 2 } ] } );
check( 1, 1, { $or: [ { b: { $gte:2,$lte:3 } }, { b: 2 } ] } );
check( 1, 1, { $or: [ { b: { $gt:2,$lte:3 } }, { b: 2 } ] } );

check( 1, 1, { $or: [ { a: { $gte:1,$lte:3 } }, { a: 2, b: 2 } ] } );
check( 1, 2, { $or: [ { a: { $gte:1,$lte:3 }, b:3 }, { a: 2 } ] } );

check( 1, 1, { $or: [ { b: { $gte:1,$lte:3 } }, { b: 2, a: 2 } ] } );
check( 1, 2, { $or: [ { b: { $gte:1,$lte:3 }, a:3 }, { b: 2 } ] } );

check( 1, 2, { $or: [ { a: { $gte:1,$lte:3 }, b: 3 }, { a: 2, b: 2 } ] } );
check( 1, 2, { $or: [ { a: { $gte:2,$lte:3 }, b: 3 }, { a: 2, b: 2 } ] } );
check( 1, 1, { $or: [ { a: { $gte:1,$lte:3 }, b: 2 }, { a: 2, b: 2 } ] } );

check( 1, 2, { $or: [ { b: { $gte:1,$lte:3 }, a: 3 }, { a: 2, b: 2 } ] } );
check( 1, 2, { $or: [ { b: { $gte:2,$lte:3 }, a: 3 }, { a: 2, b: 2 } ] } );
check( 1, 1, { $or: [ { b: { $gte:1,$lte:3 }, a: 2 }, { a: 2, b: 2 } ] } );



t.remove();

t.save( {a:1,b:5} );
t.save( {a:5,b:1} );

check( 2, 1, { $or: [ { a: { $in:[1,5] }, b: { $in:[1,5] } }, { a: { $in:[1,5] }, b: { $in:[1,5] } } ] } );
check( 2, 2, { $or: [ { a: { $in:[1] }, b: { $in:[1,5] } }, { a: { $in:[1,5] }, b: { $in:[1,5] } } ] } );
check( 2, 2, { $or: [ { a: { $in:[1] }, b: { $in:[1] } }, { a: { $in:[1,5] }, b: { $in:[1,5] } } ] } );

assert.eq.automsg( {a:[[1,1],[5,5]],b:[[1,1],[5,5]]}, "t.find( { $or: [ { a: { $in:[1] }, b: { $in:[1] } }, { a: { $in:[1,5] }, b: { $in:[1,5] } } ] } ).explain().clauses[ 1 ].indexBounds" );
