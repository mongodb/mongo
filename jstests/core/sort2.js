// test sorting, mainly a test ver simple with no index

t = db.sort2;

t.drop();
t.save({x: 1, y: {a: 5, b: 4}});
t.save({x: 1, y: {a: 7, b: 3}});
t.save({x: 1, y: {a: 2, b: 3}});
t.save({x: 1, y: {a: 9, b: 3}});
for (var pass = 0; pass < 2; pass++) {
    var res = t.find().sort({'y.a': 1}).toArray();
    assert(res[0].y.a == 2);
    assert(res[1].y.a == 5);
    assert(res.length == 4);
    t.ensureIndex({"y.a": 1});
}
assert(t.validate().valid);

t.drop();
t.insert({x: 1});
t.insert({x: 5000000000});
t.insert({x: NaN});
t.insert({x: Infinity});
t.insert({x: -Infinity});
var good = [NaN, -Infinity, 1, 5000000000, Infinity];
for (var pass = 0; pass < 2; pass++) {
    var res = t.find({}, {_id: 0}).sort({x: 1}).toArray();
    for (var i = 0; i < good.length; i++) {
        assert(good[i].toString() == res[i].x.toString());
    }
    t.ensureIndex({x: 1});
}
