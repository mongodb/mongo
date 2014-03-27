f = db.jstests_remove5;
f.drop();

getLastError = function() {
    return db.runCommand( { getlasterror : 1 } );
}

f.remove( {} );
assert.eq( 0, getLastError().n );
f.save( {a:1} );
f.remove( {} );
assert.eq( 1, getLastError().n );
for( i = 0; i < 10; ++i ) {
    f.save( {i:i} );
}
f.remove( {} );
assert.eq( 10, getLastError().n );
assert.eq( 10, db.getPrevError().n );
assert.eq( 1, db.getPrevError().nPrev );

f.findOne();
assert.eq( 0, getLastError().n );
assert.eq( 10, db.getPrevError().n );
assert.eq( 2, db.getPrevError().nPrev );
