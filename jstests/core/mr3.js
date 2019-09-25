// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

t = db.mr3;
t.drop();

t.save({x: 1, tags: ["a", "b"]});
t.save({x: 2, tags: ["b", "c"]});
t.save({x: 3, tags: ["c", "a"]});
t.save({x: 4, tags: ["b", "c"]});

m = function(n, x) {
    x = x || 1;
    this.tags.forEach(function(z) {
        for (var i = 0; i < x; i++)
            emit(z, {count: n || 1});
    });
};

r = function(key, values) {
    var total = 0;
    for (var i = 0; i < values.length; i++) {
        total += values[i].count;
    }
    return {count: total};
};

assert.commandWorked(t.mapReduce(m, r, {out: "mr3_out"}));

assert.eq(3, db.mr3_out.find().itcount(), "A1");
assert.eq(1, db.mr3_out.count({_id: "a", "value.count": 2}), "A2");
assert.eq(1, db.mr3_out.count({_id: "b", "value.count": 3}), "A3");
assert.eq(1, db.mr3_out.count({_id: "c", "value.count": 3}), "A4");

db.mr3_out.drop();

assert.commandWorked(t.mapReduce(m, r, {out: "mr3_out", mapparams: [2, 2]}));

assert.eq(3, db.mr3_out.find().itcount(), "B1");
assert.eq(1, db.mr3_out.count({_id: "a", "value.count": 8}), "B2");
assert.eq(1, db.mr3_out.count({_id: "b", "value.count": 12}), "B3");
assert.eq(1, db.mr3_out.count({_id: "c", "value.count": 12}), "B4");

db.mr3_out.drop();

// -- just some random tests

realm = m;

m = function() {
    emit(this._id, 1);
};
assert.commandWorked(t.mapReduce(m, r, {out: "mr3_out"}));
db.mr3_out.drop();

m = function() {
    emit(this._id, this.xzz.a);
};

before = db.getCollectionNames().length;
assert.throws(function() {
    t.mapReduce(m, r, {out: "mr3_out"});
});
assert.eq(before, db.getCollectionNames().length, "after throw crap");

m = realm;
r = function(k, v) {
    return v.x.x.x;
};
before = db.getCollectionNames().length;
assert.throws(function() {
    t.mapReduce(m, r, "mr3_out");
});
assert.eq(before, db.getCollectionNames().length, "after throw crap");
