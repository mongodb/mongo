// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop, requires_fastcount]

// ensure a document with _id undefined cannot be saved
let t = db.insert_id_undefined;
t.drop();
t.insert({_id: undefined});
assert.eq(t.count(), 0);
// Make sure the collection was not created
assert.isnull(t.exists());
