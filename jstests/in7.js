t = db.jstests_slow_in1;

t.drop();
t.ensureIndex( {a:1,b:1,c:1,d:1,e:1,f:1,g:1} );
i = {$in:[ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 ]};
assert.throws.automsg( function() { t.count( {a:i,b:i,c:i,d:i,e:i,f:i,g:i} ); } );
