// Test $in regular expressions with overlapping index bounds.  SERVER-4677

t = db.jstests_inb;
t.drop();

function checkBoundsAndResults( query ) {
    assert.eq( [ 'a', 'b' ], t.find( query ).explain().indexBounds.x[0] );
    assert.eq( 4, t.count( query ) );
    assert.eq( 4, t.find( query ).itcount() );
}

t.ensureIndex( {x:1} );
t.save( {x:'aa'} );
t.save( {x:'ab'} );
t.save( {x:'ac'} );
t.save( {x:'ad'} );

checkBoundsAndResults( {x:{$in:[/^a/,/^ab/]}} );
checkBoundsAndResults( {x:{$in:[/^ab/,/^a/]}} );
