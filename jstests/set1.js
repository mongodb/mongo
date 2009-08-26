
t = db.set1;
t.drop();

t.insert( { _id : 1, emb : {} });

t.update( { _id : 1 }, { $set : { emb : { 'a.dot' : 'data'} }});
assert.eq( 1 , t.findOne().emb.keySet().length , "A" );
assert.eq( "a.dot" , t.findOne().emb.keySet()[0] , "B" );

t.update( { _id : 1 }, { $set : { 'emb.b' : { dot : 'data'} }});
assert.eq( 2 , t.findOne().emb.keySet().length , "C" );
//assert.eq( "a.dot" , t.findOne().emb.keySet()[0] , "D" );
//assert.eq( "b" , t.findOne().emb.keySet()[0] , "E" );


