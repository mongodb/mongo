// SERVER-8088: test $unwind with a scalar

t = db.agg_unwind;
t.drop();

t.insert({_id: 1});
t.insert({_id: 2, x: null});
t.insert({_id: 3, x: []});
t.insert({_id: 4, x: [1, 2]});
t.insert({_id: 5, x: [3]});
t.insert({_id: 6, x: 4});

var res = t.aggregate([{$unwind: "$x"}, {$sort: {_id: 1}}]).toArray();
assert.eq(4, res.length);
assert.eq([1, 2, 3, 4], res.map(function(z) {
    return z.x;
}));
