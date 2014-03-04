// ensure a document with _id undefined cannot be saved
t = db.insert_id_undefined;
t.drop();
t.insert({_id:undefined});
assert.eq(t.count(), 0);

//TODO: b/c of SERVER-12993 - remove below line once the bug which 
//causes the server to create the collection is fixed
t.drop()