
t = db.update_setOnInsert;

db.setProfilingLevel( 2 );

function getLastOp() {
    var cursor = db.system.profile.find( { ns : t.getFullName() , op : "update" } );
    cursor = cursor.sort( { $natural : -1 } ).limit(1);
    return cursor[0];
}

function dotest( useIndex ) {
    t.drop();
    if ( useIndex ) {
        t.ensureIndex( { a : 1 } );
    }

    t.update( { _id: 5 }, { $inc : { x: 2 }, $setOnInsert : { a : 3 } }, true );
    assert.docEq( { _id : 5, a: 3, x : 2 }, t.findOne() );

    t.update( { _id: 5 }, { $set : { a : 4 } }, true );

    t.update( { _id: 5 }, { $inc : { x: 2 }, $setOnInsert : { a : 3 } }, true );
    assert.docEq( { _id : 5, a: 4, x : 4 }, t.findOne() );

    op = getLastOp();
    assert( op.fastmod );
}

dotest( false );
dotest( true );
