// This test checks validation of the replaced doc (on the server) for dots, $prefix and _id

// Create a new connection object so it won't affect the global connection when we modify
// it's settings.
var conn = new Mongo(db.getMongo().host);
conn.forceWriteMode(db.getMongo().writeMode());
t = conn.getDB(db.getName()).jstests_update_replace;
t.drop();

var myDB = t.getDB();
var res;

// Bypass validation in shell so we can test the server.
conn._skipValidation = true;

// Should not allow "." in field names
res = t.save({_id:1, "a.a":1})
assert(res.hasWriteErrors(), "a.a");

// Should not allow "." in field names, embedded
res = t.save({_id:1, a :{"a.a":1}})
assert(res.hasWriteErrors(), "a: a.a");

// Should not allow "$"-prefixed field names, caught before "." check
res = t.save({_id:1, $a :{"a.a":1}})
assert(res.hasWriteErrors(), "$a: a.a");

// Should not allow "$"-prefixed field names
res = t.save({_id:1, $a: 1})
assert(res.hasWriteErrors(), "$a");

// _id validation checks

// Should not allow regex _id
res = t.save({_id: /a/})
assert(res.hasWriteErrors(), "_id regex");

// Should not allow regex _id, even if not first
res = t.save({a:2, _id: /a/})
assert(res.hasWriteErrors(), "a _id regex");

// Should not allow array _id
res = t.save({_id: [9]})
assert(res.hasWriteErrors(), "_id array");

// This is fine since _id isn't a top level field
res = t.save({a :{ _id: [9]}})
assert(!res.hasWriteErrors(), "embedded _id array");

// This is fine since _id isn't a top level field
res = t.save({b:1, a :{ _id: [9]}})
assert(!res.hasWriteErrors(), "b embedded _id array");
