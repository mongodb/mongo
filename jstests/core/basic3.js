// Tests that "." cannot be in field names
t = db.getCollection("foo_basic3");
t.drop();

// more diagnostics on bad save, if exception fails
doBadSave = function(param) {
    print("doing save with " + tojson(param));
    var res = t.save(param);
    // Should not get here.
    printjson(res);
};

// more diagnostics on bad save, if exception fails
doBadUpdate = function(query, update) {
    print("doing update with " + tojson(query) + " " + tojson(update));
    var res = t.update(query, update);
    // Should not get here.
    printjson(res);
};

assert.throws(doBadSave, [{"a.b": 5}], ". in names aren't allowed doesn't work");

assert.throws(doBadSave, [{"x": {"a.b": 5}}], ". in embedded names aren't allowed doesn't work");

// following tests make sure update keys are checked
t.save({"a": 0, "b": 1});

assert.throws(doBadUpdate, [{a: 0}, {"b.b": 1}], "must deny '.' in key of update");

// upsert with embedded doc
assert.throws(doBadUpdate, [{a: 10}, {c: {"b.b": 1}}], "must deny embedded '.' in key of update");

// if it is a modifier, it should still go through
t.update({"a": 0}, {$set: {"c.c": 1}});
t.update({"a": 0}, {$inc: {"c.c": 1}});

// edge cases
assert.throws(
    doBadUpdate, [{a: 0}, {"": {"b.b": 1}}], "must deny '' embedded '.' in key of update");
t.update({"a": 0}, {});
