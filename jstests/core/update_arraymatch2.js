t = db.update_arraymatch2;
t.drop();

t.insert({});
t.insert({x: [1, 2, 3]});
t.insert({x: 99});
t.update({x: 2}, {$inc: {"x.$": 1}}, false, true);
assert(t.findOne({x: 1}).x[1] == 3, "A1");

t.insert({x: {y: [8, 7, 6]}});
t.update({'x.y': 7}, {$inc: {"x.y.$": 1}}, false, true);
assert.eq(8, t.findOne({"x.y": 8}).x.y[1], "B1");

t.insert({x: [90, 91, 92], y: ['a', 'b', 'c']});
t.update({x: 92}, {$set: {'y.$': 'z'}}, false, true);
assert.eq('z', t.findOne({x: 92}).y[2], "B2");
