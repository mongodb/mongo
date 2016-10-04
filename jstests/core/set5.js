
t = db.set5;
t.drop();

function check(want, err) {
    var x = t.findOne();
    delete x._id;
    assert.docEq(want, x, err);
}

t.update({a: 5}, {$set: {a: 6, b: null}}, true);
check({a: 6, b: null}, "A");

t.drop();

t.update({z: 5}, {$set: {z: 6, b: null}}, true);
check({b: null, z: 6}, "B");
