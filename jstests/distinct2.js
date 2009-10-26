
t = db.distinct2;
t.drop();

t.save({a:null});
assert.eq( 0 , t.distinct('a.b').length , "A" );
