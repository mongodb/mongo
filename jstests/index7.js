// index7.js Test that we use an index when and only when we expect to.

function index( q ) {
    assert( q.explain().cursor.match( /^BtreeCursor/ ) , "index assert" );
}

function noIndex( q ) {
    assert( q.explain().cursor.match( /^BasicCursor/ ) , "noIndex assert" );
}

function start( k, q, rev) {
    var exp = q.explain().indexBounds;
    var s = {a:exp.a[rev?1:0][0],b:exp.b[0][0]};
    assert.eq( k.a, s.a );
    assert.eq( k.b, s.b );
}
function end( k, q, rev) {
    var exp = q.explain().indexBounds
    var e = {a:exp.a[rev?1:0][1],b:exp.b[0][1]};
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
index( f.find( { a: 5 } ).sort( { a: 1 } ).hint( { a: 1 } ) );
noIndex( f.find( { a: 5 } ).sort( { a: 1 } ).hint( { $natural: 1 } ) );
f.drop();

f.ensureIndex( { a: 1, b: 1 } );
assert.eq( 1, f.find( { a: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.a[0][0] );
assert.eq( 1, f.find( { a: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.a[0][1] );
assert.eq( 1, f.find( { a: 1, c: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.a[0][0] );
assert.eq( 1, f.find( { a: 1, c: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.a[0][1] );
assert.eq( null, f.find( { a: 1, c: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.c );
assert.eq( null, f.find( { a: 1, c: 1 } ).hint( { a: 1, b: 1 } ).explain().indexBounds.c );

start( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ).hint( { a: 1, b: 1 } ) );
start( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: 1, b: 1 } ).hint( { a: 1, b: 1 } ) );
start( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: -1, b: -1 } ).hint( { a: 1, b: 1 } ), true );
start( { a: "a", b: 1 }, f.find( { b: 1, a: /^a/ } ).hint( { a: 1, b: 1 } ) );
end( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ).hint( { a: 1, b: 1 } ) );
end( { a: "b", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: 1, b: 1 } ).hint( { a: 1, b: 1 } ) );
end( { a: "a", b: 1 }, f.find( { a: /^a/, b: 1 } ).sort( { a: -1, b: -1 } ).hint( { a: 1, b: 1 } ), true );
end( { a: "b", b: 1 }, f.find( { b: 1, a: /^a/ } ).hint( { a: 1, b: 1 } ) );

start( { a: "z", b: 1 }, f.find( { a: /^z/, b: 1 } ).hint( { a: 1, b: 1 } ) );
end( { a: "{", b: 1 }, f.find( { a: /^z/, b: 1 } ).hint( { a: 1, b: 1 } ) );

start( { a: "az", b: 1 }, f.find( { a: /^az/, b: 1 } ).hint( { a: 1, b: 1 } ) );
end( { a: "a{", b: 1 }, f.find( { a: /^az/, b: 1 } ).hint( { a: 1, b: 1 } ) );

both( { a: 1, b: 3 }, f.find( { a: 1, b: 3 } ).hint( { a: 1, b: 1 } ) );

both( { a: 1, b: 2 }, f.find( { a: { $gte: 1, $lte: 1 }, b: 2 } ).hint( { a: 1, b: 1 } ) );
both( { a: 1, b: 2 }, f.find( { a: { $gte: 1, $lte: 1 }, b: 2 } ).sort( { a: 1, b: 1 } ).hint( { a: 1, b: 1 } ) );

f.drop();
f.ensureIndex( { b: 1, a: 1 } );
both( { a: 1, b: 3 }, f.find( { a: 1, b: 3 } ).hint( { b: 1, a: 1 } ) );
