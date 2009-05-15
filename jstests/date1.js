
t = db.date1;
t.drop();

d = new Date()
t.save( { a : 1 , d : d } );

assert.eq( d , t.findOne().d , "A" )
