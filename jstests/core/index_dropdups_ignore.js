// SERVER-14710 dropDups is ignored and stripped from the spec when building an index.

var t = db.index_dropdups_ignore;
t.drop();

t.insert({_id: 1, a: 'dup'});
t.insert({_id: 2, a: 'dup'});

// Should fail with a dup-key error even though dropDups is true;
var res = t.ensureIndex({a: 1}, {unique: true, dropDups: true});
assert.commandFailed(res);
assert.eq(res.code, 11000, tojson(res));

// Succeeds with the dup manually removed.
t.remove({_id: 2});
var res = t.ensureIndex({a: 1}, {unique: true, dropDups: true});
assert.commandWorked(res);

// The spec should have been stripped of the dropDups option.
t.getIndexSpecs().forEach(function(spec) {
    assert(!('dropDups' in spec), tojson(spec));
});
