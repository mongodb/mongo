
function runTest() {
    t = db.index_filtered4;
    t.drop();

    t.ensureIndex( { x : 1 } , { filter : { a : 1 } } );

    function getNumKeys() {
        var res = t.validate(true);
        return res.keysPerIndex[t.getFullName() + ".$x_1"];
    }

    t.insert( { _id : 1, x : 5, a : 2 } ); // not in index
    t.insert( { _id : 2, x : 6, a : 1 } ); // in index

    assert.eq(2, t.count());
    assert.eq(1, getNumKeys());

    t.update( { _id : 1 } , { $set : { a : 1 } } );
    assert.eq(2, getNumKeys());

    t.update( { _id : 1 } , { $set : { a : 2 } } );
    assert.eq(1, getNumKeys());

    t.remove( { _id : 1 } );
    assert.eq(1, getNumKeys());
    assert.eq(1, t.count());
}

if (db.serverStatus().process != "mongos") {
    runTest();
}
