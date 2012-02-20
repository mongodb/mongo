// Check that a projection is applied after an in memory sort.

t = db.jstests_sorti;
t.drop();

t.save( { a:1, b:0 } );
t.save( { a:3, b:1 } );
t.save( { a:2, b:2 } );
t.save( { a:4, b:3 } );

function checkBOrder( query ) {
    arr = query.toArray();
    order = [];
    for( i in arr ) {
        a = arr[ i ];
        order.push( a.b );
    }
    assert.eq( [ 0, 2, 1, 3 ], order );
}

checkBOrder( t.find().sort( { a:1 } ) );
checkBOrder( t.find( {}, { _id:0, b:1 } ).sort( { a:1 } ) );
t.ensureIndex( { b:1 } );
checkBOrder( t.find( {}, { _id:0, b:1 } ).sort( { a:1 } ) );
checkBOrder( t.find( {}, { _id:0, b:1 } ).sort( { a:1 } ).hint( { b:1 } ) );
