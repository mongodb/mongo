// index7.js Test that we use an index when and only when we expect to.

function index( q ) {
    assert( q.explain().cursor.match( /^BtreeCursor/ ) , "index assert" );
}

function noIndex( q ) {
    assert( q.explain().cursor.match( /^BasicCursor/ ) , "noIndex assert" );
}

function start( k, q ) {
    var s = q.explain().startKey;
    assert.eq( k.a, s.a );
    assert.eq( k.b, s.b );
}

function end( k, q ) {
    var e = q.explain().endKey;
    assert.eq( k.a, e.a );
    assert.eq( k.b, e.b );
}

function both( k, q ) {
    start( k, q );
    end( k, q );
}

f = db.ed_db_index7;
f.drop();

f.save( { a : 5 } )
f.ensureIndex( { a: 1 } );
index( f.find().sort( { a: 1 } ) );
index( f.find( { a: 5 } ) );
index( f.find( { a: 5 } ).sort( { a: 1 } ) );
index( f.find( { a: { $gt: 5 } } ) );
index( f.find( { a: { $gt: 5 } } ).sort( { a: 1 } ) );
index( f.find( { a: { $gt: 5, $lt: 1000 } } ) );
index( f.find( { a: { $gt: 5, $lt: 1000 } } ).sort( { a: 1 } ) );
f.drop();

f.ensureIndex( { a: 1, b: 1 } );
index( f.find().sort( { a: 1, b: 1 } ) );
index( f.find().sort( { a: -1, b: -1 } ) );
noIndex( f.find().sort( { a: 1, b: -1 } ) );
noIndex( f.find().sort( { b: 1, a: 1 } ) );

noIndex( f.find() );
noIndex( f.find( { c: 1 } ) );
noIndex( f.find( { a: 1 } ) ); // Once we enhance the query optimizer, this should get an index.
index( f.find( { a: 1, b: 1 } ) );
index( f.find( { b: 1, a: 1 } ) );

index( f.find( { a: /^a/, b: 1 } ) );
index( f.find( { b: 1, a: /^a/ } ) );
start( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ) );
start( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: 1, b: 1 } ) );
start( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: -1, b: -1 } ) );
start( { a: "a", b: 1 }, f.find( { b: 1, a: /^a/ } ) );
end( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ) );
end( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: 1, b: 1 } ) );
end( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: -1, b: -1 } ) );
end( { a: "b", b: 1 }, f.find( { b: 1, a: /^a/ } ) );

start( { a: "z", b: 1 }, f.find( { a: /^z/, b: 1 } ) );
end( { a: "{", b: 1 }, f.find( { a: /^z/, b: 1 } ) );
noIndex( f.find( { a: /^\{/, b: 1 } ) );

start( { a: "az", b: 1 }, f.find( { a: /^az/, b: 1 } ) );
end( { a: "a{", b: 1 }, f.find( { a: /^az/, b: 1 } ) );

noIndex( f.find( { a: /ab/, b: 1 } ) );
noIndex( f.find( { a: /^ab/g, b: 1 } ) );
noIndex( f.find( { a: /^ab-/, b: 1 } ) );
noIndex( f.find( { b: 1, a: /ab/ } ) );
index( f.find( { b: /ab/, a: 1 } ) );
index( f.find( { a: 1, b: /ab/ } ) );

noIndex( f.find( { a: { $in: [ 1, 2 ] }, b: 1 } ) );
index( f.find( { a: 1, b: { $in: [ 1, 2 ] } } ) );

noIndex( f.find( { a: /^/, b: 1 } ) );

both( { a: 1, b: 3 }, f.find( { a: 1, b: 3 } ) );

index( f.find( { a: { $gt: 1, $lt: 1 }, b: 2 } ) );
both( { a: 1, b: 2 }, f.find( { a: { $gt: 1, $lt: 1 }, b: 2 } ) );
index( f.find( { a: { $gt: 1, $lt: 1 }, b: 2 } ).sort( { a: 1, b: 1 } ) );
both( { a: 1, b: 2 }, f.find( { a: { $gt: 1, $lt: 1 }, b: 2 } ).sort( { a: 1, b: 1 } ) );

index( f.find( { a: { $gt: 1 }, b: { $lt: 2 } } ) );
index( f.find( { a: { $gt: 1 }, b: { $lt: 2 } } ).sort( { a: 1, b: 1 } ) );

f.drop();
f.ensureIndex( { b: 1, a: 1 } );
both( { a: 1, b: 3 }, f.find( { a: 1, b: 3 } ) );
