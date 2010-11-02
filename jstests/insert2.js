
t = db.insert2
t.drop()

assert.isnull( t.findOne() , "A" )
t.insert( { z : 1 ,  $inc : { x : 1 } } , true );
assert.isnull( t.findOne() , "B" )

