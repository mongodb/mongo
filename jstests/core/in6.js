t = db.jstests_in6;
t.drop();

t.save({});

function doTest() {
    assert.eq.automsg("1", "t.count( {i:null} )");
    assert.eq.automsg("1", "t.count( {i:{$in:[null]}} )");
}

doTest();
t.ensureIndex({i: 1});
doTest();
