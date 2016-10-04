
t = db.indexd;
t.drop();

t.save({a: 1});
t.ensureIndex({a: 1});
assert.throws(function() {
    db.indexd.$_id_.drop();
});
assert(t.drop());

// db.indexd.$_id_.remove({});
