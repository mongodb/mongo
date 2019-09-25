// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   uses_map_reduce_with_temp_collections,
// ]

t = db.mr_bigobject;
t.drop();

var large = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
var s = large;
while (s.length < (6 * 1024 * 1024)) {
    s += large;
}

for (i = 0; i < 5; i++)
    t.insert({_id: i, s: s});

m = function() {
    emit(1, this.s + this.s);
};

r = function(k, v) {
    return 1;
};

assert.throws(function() {
    r = t.mapReduce(m, r, "mr_bigobject_out");
}, [], "emit should fail");

m = function() {
    emit(1, this.s);
};
assert.commandWorked(t.mapReduce(m, r, "mr_bigobject_out"));
assert.eq([{_id: 1, value: 1}], db.mr_bigobject_out.find().toArray(), "A1");

r = function(k, v) {
    total = 0;
    for (var i = 0; i < v.length; i++) {
        var x = v[i];
        if (typeof (x) == "number")
            total += x;
        else
            total += x.length;
    }
    return total;
};

assert.commandWorked(t.mapReduce(m, r, "mr_bigobject_out"));
assert.eq([{_id: 1, value: t.count() * s.length}], db.mr_bigobject_out.find().toArray(), "A1");

t.drop();
