// ensure a document with _id undefined cannot be saved
t = db.insert_id_undefined;
t.drop();
t.insert({_id:undefined});
db.getLastError();
assert.eq(t.count(), 0);
