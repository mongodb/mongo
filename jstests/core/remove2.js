// @tags: [requires_non_retryable_writes]

// remove2.js
// a unit test for db remove
(function() {
"use strict";

const t = db.removetest2;

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

function g() {
    t.save({x: [3, 4, 5, 6], z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});
    t.save({x: [7, 8, 9], z: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"});

    const res = t.remove({x: {$gte: 3}});

    assert.writeOK(res);
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
})();
