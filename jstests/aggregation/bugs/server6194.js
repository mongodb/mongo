// make sure $concat doesn't optimize constants to the end
c = db.c;
c.drop();

c.save( { x:3 } );

project = { $project:{ a:{ $concat:[ 1, 2, { $concat:[ 'foo', '$x' ] } ] } } };

assert.eq( '12foo3', c.aggregate( project ).result[ 0 ].a );
