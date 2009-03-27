f = db.jstests_update4;
f.drop();

getLastError = function() {
    ret = db.runCommand( { getlasterror : 1 } );
//    printjson( ret );
    return ret;
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
assert.eq( true, db.getPrevError().updatedExisting );
assert.eq( 1, db.getPrevError().nPrev );

f.findOne();
assert.eq( undefined, getLastError().updatedExisting );
assert.eq( true, db.getPrevError().updatedExisting );
assert.eq( 2, db.getPrevError().nPrev );

db.forceError();
assert.eq( undefined, getLastError().updatedExisting );
