
t = db.set1;
t.drop();

t.insert( { _id : 1, emb : {} });

t.update( { _id : 1 }, { $set : { emb : { 'a.dot' : 'data'} }});
assert.eq( 1 , Object.keySet( t.findOne().emb ).length , "A" );
assert.eq( "a.dot" , Object.keySet( t.findOne().emb )[0] , "B" );

t.update( { _id : 1 }, { $set : { 'emb.b' : { dot : 'data'} }});
assert.eq( 2 , Object.keySet( t.findOne().emb ).length , "C" );
//assert.eq( "a.dot" , Object.keySet( t.findOne().emb )[0] , "D" );
//assert.eq( "b" , Obkect.keySet( t.findOne().emb )[0] , "E" );


