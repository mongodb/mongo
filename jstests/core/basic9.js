// Tests that $<prefix> field names are not allowed, but you can use a $ anywhere else.
t = db.getCollection("foo_basic9");
t.drop();

// more diagnostics on bad save, if exception fails
doBadSave = function(param) {
    print("doing save with " + tojson(param));
    var res = t.save(param);
    // Should not get here.
    print('Should have errored out: ' + tojson(res));
};

t.save({foo$foo: 5});
t.save({foo$: 5});

assert.throws(doBadSave, [{$foo: 5}], "key names aren't allowed to start with $ doesn't work");
assert.throws(
    doBadSave, [{x: {$foo: 5}}], "embedded key names aren't allowed to start with $ doesn't work");
