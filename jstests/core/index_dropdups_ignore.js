// Cannot implicitly shard accessed collections because of not being able to create unique index
// using hashed shard key pattern.
// @tags: [cannot_create_unique_index_when_using_hashed_shard_key, requires_non_retryable_writes]

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
