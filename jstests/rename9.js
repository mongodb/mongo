// SERVER-12886 Tests for an internal server error caused by the code change.

t = db.jstests_rename9;
t.drop();

// new _id from insert (upsert:true)
t.update({a:1}, {$inc:{b:1}}, true)
var doc = t.findOne({a:1});
assert(doc["_id"], "missing _id")

// new _id from insert (upsert:true)
t.update({a:1}, {$inc:{b:1}}, true)
var doc = t.findOne({a:1});
assert(doc["_id"], "missing _id")

// no _id on existing doc
t.getDB().runCommand({godinsert:t.getName(), obj:{a:2}})
t.update({a:2}, {$inc:{b:1}}, true)
var doc = t.findOne({a:2});
assert(doc["_id"], "missing _id after update")
