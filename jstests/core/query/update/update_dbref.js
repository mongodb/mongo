// Test that we can update DBRefs, but not dbref fields outside a DBRef

let res;
const t = db[jsTestName()];
t.drop();

res = t.save({_id: 1, a: new DBRef("a", "b")});
assert(!res.hasWriteError(), "failed to save dbref");
assert.docEq({_id: 1, a: new DBRef("a", "b")}, t.findOne());

res = t.update({}, {$set: {"a.$id": 2}});
assert(!res.hasWriteError(), "a.$id update");
assert.docEq({_id: 1, a: new DBRef("a", 2)}, t.findOne());

res = t.update({}, {$set: {"a.$ref": "b"}});
assert(!res.hasWriteError(), "a.$ref update");

assert.docEq({_id: 1, a: new DBRef("b", 2)}, t.findOne());

// Bad updates
res = t.update({}, {$set: {"$id": 3}});
assert.writeError(res);
assert(/\$id/.test(res.getWriteError()), "expected bad update because of $id");
assert.docEq({_id: 1, a: new DBRef("b", 2)}, t.findOne());

res = t.update({}, {$set: {"$ref": "foo"}});
assert.writeError(res);
assert(/\$ref/.test(res.getWriteError()), "expected bad update because of $ref");
assert.docEq({_id: 1, a: new DBRef("b", 2)}, t.findOne());

res = t.update({}, {$set: {"$db": "aDB"}});
assert.writeError(res);
assert(/\$db/.test(res.getWriteError()), "expected bad update because of $db");
assert.docEq({_id: 1, a: new DBRef("b", 2)}, t.findOne());
