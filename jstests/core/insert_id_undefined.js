// ensure a document with _id undefined cannot be saved
t = db.insert_id_undefined;
t.drop();
t.insert({_id: undefined});
assert.eq(t.count(), 0);
// Make sure the collection was not created
assert.commandFailed(t.stats());
