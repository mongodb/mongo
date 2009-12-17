
t = db.update8;
t.drop();

t.update( { _id : 1 , tags: {"$ne": "a"}}, {"$push": { tags : "a" } } , true )
le = db.getLastError()
assert.isnull( le , le );

