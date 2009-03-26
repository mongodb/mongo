f = db.jstests_update4;
f.drop();

getLastError = function() {
    return db.runCommand( { getlasterror : 1 } );
}

f.save( {a:1} );
f.update( {a:1}, {a:2} );
assert.eq( true, getLastError().updatedExisting );
f.update( {a:1}, {a:2} );
assert.eq( false, getLastError().updatedExisting );

f.update( {a:1}, {a:1}, true );
assert.eq( false, getLastError().updatedExisting );
f.update( {a:1}, {a:1}, true );
assert.eq( true, getLastError().updatedExisting );
