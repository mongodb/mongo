// SERVER-2343 Test $in empty array matching.

t = db.jstests_in9;
t.drop();

function someData() {
    t.remove({});
    t.save({key: []});
}

function moreData() {
    someData();
    t.save({key: [1]});
    t.save({key: ['1']});
    t.save({key: null});
    t.save({});
}

function check() {
    assert.eq(1, t.count({key: []}));
    assert.eq(1, t.count({key: {$in: [[]]}}));
}

function doTest() {
    someData();
    check();
    moreData();
    check();
}

doTest();

// SERVER-1943 not fixed yet
t.ensureIndex({key: 1});
doTest();
