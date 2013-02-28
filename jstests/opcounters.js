// Test that opcounters get incremented properly.

db.dropDatabase();

var original = db.serverStatus().opcounters;
var current;

printjson(original);

// Command
db.foo.count();
current = db.serverStatus().opcounters;
assert.eq(current.command, original.command + 2); // One for count one for serverStatus

// Insert
db.foo.insert({a:1});
current = db.serverStatus().opcounters;
assert.eq(current.insert, original.insert + 1);
db.foo.insert([{a:2}, {a:3}]);
current = db.serverStatus().opcounters;
assert.eq(current.insert, original.insert + 3); // Batch-inserts should be counted as multiple inserts

// Update
db.foo.update({a:1}, {$set : {b:1}});
current = db.serverStatus().opcounters;
assert.eq(current.update, original.update + 1);
db.foo.update({}, {$inc : {b:1}}, false, true);
current = db.serverStatus().opcounters;
assert.eq(current.update, original.update + 2); // Multi-updates are counted as 1 update

// Query
db.foo.findOne({a:1});
current = db.serverStatus().opcounters;
assert.eq(current.query, original.query + 1);
db.foo.find().toArray();
current = db.serverStatus().opcounters;
assert.eq(current.query, original.query + 2);

// Delete
db.foo.remove({a:1});
current = db.serverStatus().opcounters;
assert.eq(current.delete, original.delete + 1);
db.foo.remove({});
current = db.serverStatus().opcounters;
assert.eq(current.delete, original.delete + 2); // Multi-removes are counted as 1 remove
