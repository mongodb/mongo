
t = db.arrayfind2;
t.drop();

function go(prefix) {
    assert.eq(3, t.count(), prefix + " A1");
    assert.eq(3, t.find({a: {$elemMatch: {x: {$gt: 4}}}}).count(), prefix + " A2");
    assert.eq(1, t.find({a: {$elemMatch: {x: {$lt: 2}}}}).count(), prefix + " A3");
    assert.eq(
        1,
        t.find({a: {$all: [{$elemMatch: {x: {$lt: 4}}}, {$elemMatch: {x: {$gt: 5}}}]}}).count(),
        prefix + " A4");

    assert.throws(function() {
        return t.findOne({a: {$all: [1, {$elemMatch: {x: 3}}]}});
    });
    assert.throws(function() {
        return t.findOne({a: {$all: [/a/, {$elemMatch: {x: 3}}]}});
    });
}

t.save({a: [{x: 1}, {x: 5}]});
t.save({a: [{x: 3}, {x: 5}]});
t.save({a: [{x: 3}, {x: 6}]});

go("no index");
t.ensureIndex({a: 1});
go("index(a)");
