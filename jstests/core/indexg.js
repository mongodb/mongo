
f = db.jstests_indexg;
f.drop();
f.save( { list: [1, 2] } ); 
f.save( { list: [1, 3] } ); 

doit = function() {
    assert.eq( 1, f.count( { list: { $in: [1], $ne: 3 } } ) );
    assert.eq( 1, f.count( { list: { $in: [1], $not:{$in: [3] } } } ) );
}
doit();
f.ensureIndex( { list: 1 } ); 
doit();