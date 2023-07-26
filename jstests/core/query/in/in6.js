let t = db.jstests_in6;
t.drop();

t.save({});

function doTest() {
    assert.eq(t.count({i: null}), 1);
    assert.eq(t.count({i: {$in: [null]}}), 1);
}

doTest();
t.createIndex({i: 1});
doTest();
