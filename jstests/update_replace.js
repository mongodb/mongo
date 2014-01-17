// This test checks validation of the replaced doc (on the server) for dots, $prefix and _id

// Create a new connection object so it won't affect the global connection when we modify
// it's settings.
var conn = new Mongo(db.getMongo().host);
t = conn.getDB(db.getName()).jstests_update_replace;
t.drop();

var myDB = t.getDB();

// Bypass validation in shell so we can test the server.
conn._skipValidation = true;

// Should not allow "." in field names
t.save({_id:1, "a.a":1})
assert.gleError(myDB, "a.a");

// Should not allow "." in field names, embedded
t.save({_id:1, a :{"a.a":1}})
assert.gleError(myDB, "a: a.a");

// Should not allow "$"-prefixed field names, caught before "." check
t.save({_id:1, $a :{"a.a":1}})
assert.gleError(myDB, "$a: a.a");

// Should not allow "$"-prefixed field names
t.save({_id:1, $a: 1})
assert.gleError(myDB, "$a");

// _id validation checks

// Should not allow regex _id
t.save({_id: /a/})
assert.gleError(myDB, "_id regex");

// Should not allow regex _id, even if not first
t.save({a:2, _id: /a/})
assert.gleError(myDB, "a _id regex");

// Should not allow array _id
t.save({_id: [9]})
assert.gleError(myDB, "_id array");

// This is fine since _id isn't a top level field
t.save({a :{ _id: [9]}})
assert.gleSuccess(myDB, "embedded _id array");

// This is fine since _id isn't a top level field
t.save({b:1, a :{ _id: [9]}})
assert.gleSuccess(myDB, "b embedded _id array");
