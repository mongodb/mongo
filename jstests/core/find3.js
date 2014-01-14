t = db.find3;
t.drop();

for ( i=1; i<=50; i++)
    t.save( { a : i } );

assert.eq( 50 , t.find().toArray().length );
assert.eq( 20 , t.find().limit(20).toArray().length );

assert(t.validate().valid);
