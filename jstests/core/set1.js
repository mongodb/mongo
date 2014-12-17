
t = db.set1;
t.drop();

t.insert( { _id : 1, emb : {} });
t.update( { _id : 1 }, { $set : { emb : { 'a.dot' : 'data'} }});
assert.eq( { _id : 1 , emb : {} } , t.findOne() , "A" );


