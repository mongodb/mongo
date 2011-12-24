t = db.keydedup;
t.drop();

t.save( {a:5} );
t.save( {a:NumberLong(5)} );
t.ensureIndex( {a:1} );
assert.eq( {a:5}, t.find( {a:5 }, {_id:0, a:1} ).toArray()[ 0 ] );
assert.eq( {a:NumberLong(5)}, t.find( {a:5 }, {_id:0, a:1} ).toArray()[ 1 ] );
