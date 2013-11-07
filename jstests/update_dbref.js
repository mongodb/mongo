// Test that we can update DBRefs, but not dbref fields outside a DBRef

t = db.jstests_update_dbref;
t.drop();

t.save({_id:1, a: new DBRef("a", "b")});
assert.docEq({_id:1, a: new DBRef("a", "b")}, t.findOne());

t.update({}, {$set: {"a.$id": 2}});
assert.gleSuccess(db, "a.$id update");
assert.docEq({_id:1, a: new DBRef("a", 2)}, t.findOne());

t.update({}, {$set: {"a.$ref": "b"}});
assert.gleSuccess(db, "a.$ref update");

assert.docEq({_id:1, a: new DBRef("b", 2)}, t.findOne());

// Bad updates
t.update({}, {$set: {"$id": 3}});
assert.gleErrorRegex(db, /\$id/, "expected bad update because of $id")
assert.docEq({_id:1, a: new DBRef("b", 2)}, t.findOne());

t.update({}, {$set: {"$ref": "foo"}});
assert.gleErrorRegex(db, /\$ref/, "expected bad update because of $ref")
assert.docEq({_id:1, a: new DBRef("b", 2)}, t.findOne());

t.update({}, {$set: {"$db": "aDB"}});
assert.gleErrorRegex(db, /\$db/, "expected bad update because of $db")
assert.docEq({_id:1, a: new DBRef("b", 2)}, t.findOne());

t.update({}, {$set: {"b.$id": 2}});
assert.gleError(db, function() { return "b.$id update -- doc:" + tojson(t.findOne())});

t.update({}, {$set: {"b.$ref": 2}});
assert.gleError(db, function() { return "b.$ref update -- doc:" + tojson(t.findOne())});
