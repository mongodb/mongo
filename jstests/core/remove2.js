// remove2.js
// a unit test for db remove

t = db.removetest2;

function f() {
    t.save({
        x: [3, 3, 3, 3, 3, 3, 3, 3, 4, 5, 6],
        z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    });
    t.save({x: 9});
    t.save({x: 1});

    t.remove({x: 3});

    assert(t.findOne({x: 3}) == null);
    assert(t.validate().valid);
}

x = 0;

function g() {
    t.save({x: [3, 4, 5, 6], z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});
    t.save({x: [7, 8, 9], z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});

    var res;
    res = t.remove({x: {$gte: 3}, $atomic: x++});

    assert.writeOK(res);
    // $atomic within $and is not allowed.
    // res = t.remove( {x : {$gte:3}, $and:[{$atomic:true}] } );
    // assert.writeError( res );

    assert(t.findOne({x: 3}) == null);
    assert(t.findOne({x: 8}) == null);
    assert(t.validate().valid);
}

t.drop();
f();
t.drop();
g();

t.ensureIndex({x: 1});
t.remove({});
f();
t.drop();
t.ensureIndex({x: 1});
g();
