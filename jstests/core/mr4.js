
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

res = t.mapReduce(m, r, {out: "mr4_out", scope: {xx: {val: 1}}});
z = res.convertToSingleObject();

assert.eq(3, Object.keySet(z).length, "A1");
assert.eq(2, z.a.count, "A2");
assert.eq(3, z.b.count, "A3");
assert.eq(3, z.c.count, "A4");

res.drop();

res = t.mapReduce(m, r, {scope: {xx: {val: 2}}, out: "mr4_out"});
z = res.convertToSingleObject();

assert.eq(3, Object.keySet(z).length, "A1");
assert.eq(4, z.a.count, "A2");
assert.eq(6, z.b.count, "A3");
assert.eq(6, z.c.count, "A4");

res.drop();
