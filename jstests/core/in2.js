
t = db.in2;

function go(name, index) {
    t.drop();

    t.save({a: 1, b: 1});
    t.save({a: 1, b: 2});
    t.save({a: 1, b: 3});

    t.save({a: 1, b: 1});
    t.save({a: 2, b: 2});
    t.save({a: 3, b: 3});

    t.save({a: 1, b: 1});
    t.save({a: 2, b: 1});
    t.save({a: 3, b: 1});

    if (index)
        t.ensureIndex(index);

    assert.eq(7, t.find({a: {$in: [1, 2]}}).count(), name + " A");

    assert.eq(6, t.find({a: {$in: [1, 2]}, b: {$in: [1, 2]}}).count(), name + " B");
}

go("no index");
go("index on a", {a: 1});
go("index on b", {b: 1});
go("index on a&b", {a: 1, b: 1});
