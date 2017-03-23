
t = db.bigkeysidxtest;

var keys = [];

var str = "aaaabbbbccccddddeeeeffffgggghhhh";

while (str.length < 20000) {
    keys.push(str);
    str = str + str;
}

function doInsert(order) {
    if (order == 1) {
        for (var i = 0; i < 10; i++) {
            t.insert({_id: i, k: keys[i]});
        }
    } else {
        for (var i = 9; i >= 0; i--) {
            t.insert({_id: i, k: keys[i]});
        }
    }
}

var expect = null;

function check() {
    assert(t.validate().valid);
    assert.eq(5, t.count());

    var c = t.find({k: /^a/}).count();
    assert.eq(5, c);
}

function runTest(order) {
    t.drop();
    t.ensureIndex({k: 1});
    doInsert(order);
    check();  // check incremental addition

    t.reIndex();
    check();  // check bottom up

    t.drop();
    doInsert(order);
    assert.eq(1, t.getIndexes().length);
    t.ensureIndex({k: 1});
    assert.eq(1, t.getIndexes().length);

    t.drop();
    doInsert(order);
    assert.eq(1, t.getIndexes().length);
    t.ensureIndex({k: 1}, {background: true});
    assert.eq(1, t.getIndexes().length);
}

runTest(1);
runTest(2);
