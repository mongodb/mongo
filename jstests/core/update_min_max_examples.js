// Basic examples for $min/$max
var res;
var coll = db.update_min_max;
coll.drop();

// $min for number
coll.insert({_id: 1, a: 2});
res = coll.update({_id: 1}, {$min: {a: 1}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 1}).a, 1);

// $max for number
coll.insert({_id: 2, a: 2});
res = coll.update({_id: 2}, {$max: {a: 1}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 2}).a, 2);

// $min for Date
coll.insert({_id: 3, a: new Date()});
var origDoc = coll.findOne({_id: 3});
sleep(2);
res = coll.update({_id: 3}, {$min: {a: new Date()}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 3}).a, origDoc.a);

// $max for Date
coll.insert({_id: 4, a: new Date()});
sleep(2);
var newDate = new Date();
res = coll.update({_id: 4}, {$max: {a: newDate}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 4}).a, newDate);

// $max for small number
coll.insert({_id: 5, a: 1e-15});
// Slightly bigger than 1e-15.
var biggerval = 0.000000000000001000000000000001;
res = coll.update({_id: 5}, {$max: {a: biggerval}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 5}).a, biggerval);

// $min for a small number
coll.insert({_id: 6, a: biggerval});
res = coll.update({_id: 6}, {$min: {a: 1e-15}});
assert.writeOK(res);
assert.eq(coll.findOne({_id: 6}).a, 1e-15);

// $max with positional operator
var insertdoc = {_id: 7, y: [{a: 2}, {a: 6}, {a: [9, 1, 1]}]};
coll.insert(insertdoc);
res = coll.update({_id: 7, "y.a": 6}, {$max: {"y.$.a": 7}});
assert.writeOK(res);
insertdoc.y[1].a = 7;
assert.docEq(coll.findOne({_id: 7}), insertdoc);

// $min with positional operator
insertdoc = {
    _id: 8,
    y: [{a: 2}, {a: 6}, {a: [9, 1, 1]}]
};
coll.insert(insertdoc);
res = coll.update({_id: 8, "y.a": 6}, {$min: {"y.$.a": 5}});
assert.writeOK(res);
insertdoc.y[1].a = 5;
assert.docEq(coll.findOne({_id: 8}), insertdoc);
