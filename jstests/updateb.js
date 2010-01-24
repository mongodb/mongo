
t = db.updateb;
t.drop();

t.update( { "x.y" : 2 } , { $inc : { a : 7 } } , true );

correct = { a : 7 , x : { y : 2 } };
got = t.findOne();
delete got._id;
assert.eq( correct , got , "A" )

