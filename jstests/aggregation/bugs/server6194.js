// make sure $concat doesn't optimize constants to the end
c = db.c;
c.drop();

c.save( { x:'3' } );

project = { $project:{ a:{ $concat:[ '1', { $concat:[ 'foo', '$x', 'bar' ] }, '2' ] } } };

assert.eq( '1foo3bar2', c.aggregate( project ).result[ 0 ].a );
