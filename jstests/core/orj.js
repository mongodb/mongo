// Test nested $or clauses SERVER-2585 SERVER-3192

t = db.jstests_orj;
t.drop();

t.save({a: 1, b: 2});

function check() {
    assert.throws(function() {
        t.find({x: 0, $or: "a"}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: []}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: ["a"]}).toArray();
    });

    assert.throws(function() {
        t.find({x: 0, $or: [{$or: "a"}]}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: [{$or: []}]}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $or: [{$or: ["a"]}]}).toArray();
    });

    assert.throws(function() {
        t.find({x: 0, $nor: "a"}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: []}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: ["a"]}).toArray();
    });

    assert.throws(function() {
        t.find({x: 0, $nor: [{$nor: "a"}]}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: [{$nor: []}]}).toArray();
    });
    assert.throws(function() {
        t.find({x: 0, $nor: [{$nor: ["a"]}]}).toArray();
    });

    assert.eq(1, t.find({a: 1, b: 2}).itcount());

    assert.eq(1, t.find({a: 1, $or: [{b: 2}]}).itcount());
    assert.eq(0, t.find({a: 1, $or: [{b: 3}]}).itcount());

    assert.eq(1, t.find({a: 1, $or: [{$or: [{b: 2}]}]}).itcount());
    assert.eq(1, t.find({a: 1, $or: [{$or: [{b: 2}]}]}).itcount());
    assert.eq(0, t.find({a: 1, $or: [{$or: [{b: 3}]}]}).itcount());

    assert.eq(1, t.find({$or: [{$or: [{a: 2}, {b: 2}]}]}).itcount());
    assert.eq(1, t.find({$or: [{a: 2}, {$or: [{b: 2}]}]}).itcount());
    assert.eq(1, t.find({$or: [{a: 1}, {$or: [{b: 3}]}]}).itcount());

    assert.eq(1, t.find({$or: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 3}, {b: 4}]}]}).itcount());
    assert.eq(1, t.find({$or: [{$or: [{a: 0}, {a: 2}]}, {$or: [{b: 2}, {b: 4}]}]}).itcount());
    assert.eq(0, t.find({$or: [{$or: [{a: 0}, {a: 2}]}, {$or: [{b: 3}, {b: 4}]}]}).itcount());

    assert.eq(1, t.find({a: 1, $and: [{$or: [{$or: [{b: 2}]}]}]}).itcount());
    assert.eq(0, t.find({a: 1, $and: [{$or: [{$or: [{b: 3}]}]}]}).itcount());

    assert.eq(1, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 1}, {b: 2}]}]}).itcount());
    assert.eq(0, t.find({$and: [{$or: [{a: 3}, {a: 2}]}, {$or: [{b: 1}, {b: 2}]}]}).itcount());
    assert.eq(0, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 3}, {b: 1}]}]}).itcount());

    assert.eq(0, t.find({$and: [{$nor: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).itcount());
    assert.eq(0, t.find({$and: [{$nor: [{a: 3}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).itcount());
    assert.eq(1, t.find({$and: [{$nor: [{a: 3}, {a: 2}]}, {$nor: [{b: 3}, {b: 1}]}]}).itcount());

    assert.eq(1, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 3}]}]}).itcount());
    assert.eq(0, t.find({$and: [{$or: [{a: 3}, {a: 2}]}, {$nor: [{b: 1}, {b: 3}]}]}).itcount());
    assert.eq(0, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).itcount());
}

check();

t.ensureIndex({a: 1});
check();
t.dropIndexes();

t.ensureIndex({b: 1});
check();
t.dropIndexes();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
check();
t.dropIndexes();

t.ensureIndex({a: 1, b: 1});
check();
t.dropIndexes();

t.ensureIndex({a: 1});
t.ensureIndex({b: 1});
t.ensureIndex({a: 1, b: 1});
check();

function checkHinted(hint) {
    assert.eq(1, t.find({a: 1, b: 2}).hint(hint).itcount());

    assert.eq(1, t.find({a: 1, $or: [{b: 2}]}).hint(hint).itcount());
    assert.eq(0, t.find({a: 1, $or: [{b: 3}]}).hint(hint).itcount());

    assert.eq(1, t.find({a: 1, $or: [{$or: [{b: 2}]}]}).hint(hint).itcount());
    assert.eq(1, t.find({a: 1, $or: [{$or: [{b: 2}]}]}).hint(hint).itcount());
    assert.eq(0, t.find({a: 1, $or: [{$or: [{b: 3}]}]}).hint(hint).itcount());

    assert.eq(1, t.find({$or: [{$or: [{a: 2}, {b: 2}]}]}).hint(hint).itcount());
    assert.eq(1, t.find({$or: [{a: 2}, {$or: [{b: 2}]}]}).hint(hint).itcount());
    assert.eq(1, t.find({$or: [{a: 1}, {$or: [{b: 3}]}]}).hint(hint).itcount());

    assert.eq(
        1, t.find({$or: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 3}, {b: 4}]}]}).hint(hint).itcount());
    assert.eq(
        1, t.find({$or: [{$or: [{a: 0}, {a: 2}]}, {$or: [{b: 2}, {b: 4}]}]}).hint(hint).itcount());
    assert.eq(
        0, t.find({$or: [{$or: [{a: 0}, {a: 2}]}, {$or: [{b: 3}, {b: 4}]}]}).hint(hint).itcount());

    assert.eq(1, t.find({a: 1, $and: [{$or: [{$or: [{b: 2}]}]}]}).hint(hint).itcount());
    assert.eq(0, t.find({a: 1, $and: [{$or: [{$or: [{b: 3}]}]}]}).hint(hint).itcount());

    assert.eq(
        1, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 1}, {b: 2}]}]}).hint(hint).itcount());
    assert.eq(
        0, t.find({$and: [{$or: [{a: 3}, {a: 2}]}, {$or: [{b: 1}, {b: 2}]}]}).hint(hint).itcount());
    assert.eq(
        0, t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$or: [{b: 3}, {b: 1}]}]}).hint(hint).itcount());

    assert.eq(
        0,
        t.find({$and: [{$nor: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).hint(hint).itcount());
    assert.eq(
        0,
        t.find({$and: [{$nor: [{a: 3}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).hint(hint).itcount());
    assert.eq(
        1,
        t.find({$and: [{$nor: [{a: 3}, {a: 2}]}, {$nor: [{b: 3}, {b: 1}]}]}).hint(hint).itcount());

    assert.eq(
        1,
        t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 3}]}]}).hint(hint).itcount());
    assert.eq(
        0,
        t.find({$and: [{$or: [{a: 3}, {a: 2}]}, {$nor: [{b: 1}, {b: 3}]}]}).hint(hint).itcount());
    assert.eq(
        0,
        t.find({$and: [{$or: [{a: 1}, {a: 2}]}, {$nor: [{b: 1}, {b: 2}]}]}).hint(hint).itcount());
}

checkHinted({$natural: 1});
checkHinted({a: 1});
checkHinted({b: 1});
checkHinted({a: 1, b: 1});