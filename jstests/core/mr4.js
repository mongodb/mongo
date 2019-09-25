// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]

t = db.mr4;
t.drop();

t.save({x: 1, tags: ["a", "b"]});
t.save({x: 2, tags: ["b", "c"]});
t.save({x: 3, tags: ["c", "a"]});
t.save({x: 4, tags: ["b", "c"]});

m = function() {
    this.tags.forEach(function(z) {
        emit(z, {count: xx.val});
    });
};

r = function(key, values) {
    var total = 0;
    for (var i = 0; i < values.length; i++) {
        total += values[i].count;
    }
    return {count: total};
};

assert.commandWorked(t.mapReduce(m, r, {out: "mr4_out", scope: {xx: {val: 1}}}));

assert.eq(3, db.mr4_out.find().itcount(), "A1");
assert.eq(1, db.mr4_out.count({_id: "a", "value.count": 2}), "A2");
assert.eq(1, db.mr4_out.count({_id: "b", "value.count": 3}), "A3");
assert.eq(1, db.mr4_out.count({_id: "c", "value.count": 3}), "A4");

db.mr4_out.drop();
assert.commandWorked(t.mapReduce(m, r, {scope: {xx: {val: 2}}, out: "mr4_out"}));

assert.eq(3, db.mr4_out.find().itcount(), "A1");
assert.eq(1, db.mr4_out.count({_id: "a", "value.count": 4}), "A2");
assert.eq(1, db.mr4_out.count({_id: "b", "value.count": 6}), "A3");
assert.eq(1, db.mr4_out.count({_id: "c", "value.count": 6}), "A4");

db.mr4_out.drop();
