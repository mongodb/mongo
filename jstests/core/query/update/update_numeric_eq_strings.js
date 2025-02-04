// Test modifier operations on numerically equivalent string field names.  SERVER-4776

let t = db[jsTestName()];

t.drop();
assert.commandWorked(t.save({_id: 0, '1': {}, '01': {}}));
assert.commandWorked(t.update({}, {$set: {'1.b': 1, '1.c': 2}}));
assert.docEq({"01": {}, "1": {"b": 1, "c": 2}, "_id": 0}, t.findOne());

assert(t.drop());
assert.commandWorked(t.save({_id: 0, '1': {}, '01': {}}));
assert.commandWorked(t.update({}, {$set: {'1.b': 1, '01.c': 2}}));
assert.docEq({"01": {"c": 2}, "1": {"b": 1}, "_id": 0}, t.findOne());
