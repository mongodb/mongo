t = db.push_sort;
t.drop();

t.save( { _id: 1, x: [ {a:1}, {a:2} ] } );
t.update( {_id:1}, { $push: { x: { $each: [ {a:3} ], $trimTo: 5, $sort: {a:1} } } } )
assert.eq( [{a:1}, {a:2}, {a:3}] , t.findOne( {_id:1} ).x );

t.save({ _id: 2, x: [ {a:1}, {a:3} ] } );
t.update( {_id:2}, { $push: { x: { $each: [ {a:2} ], $trimTo: 2, $sort: {a:1} } } } )
assert.eq( [{a:2}, {a:3}], t.findOne( {_id:2} ).x );

t.save({ _id: 3, x: [ {a:1}, {a:3} ] } );
t.update( {_id:3}, { $push: { x: { $each: [ {a:2} ], $trimTo: 5, $sort: {a:-1} } } } )
assert.eq( [{a:3}, {a:2}, {a:1}], t.findOne( {_id:3} ).x );

t.save({ _id: 4, x: [ {a:1}, {a:3} ] } );
t.update( {_id:4}, { $push: { x: { $each: [ {a:2} ], $trimTo: 2, $sort: {a:-1} } } } )
assert.eq( [{a:2}, {a:1}], t.findOne( {_id:4} ).x );

t.save({ _id: 5, x: [ {a:1,b:2}, {a:3,b:1} ] } );
t.update( {_id:5}, { $push: { x: { $each: [ {a:2,b:3} ], $trimTo: 2, $sort: {b:1} } } } )
assert.eq( [{a:1, b:2}, {a:2,b:3}], t.findOne( {_id:5} ).x );

t.save({ _id: 5, x: [ {a:1,b:2}, {a:3,b:1} ] } );
t.update( {_id:5}, { $push: { x: { $each: [ {a:2,b:3} ], $trimTo: 2, $sort: {b:1} } } } )
assert.eq( [{a:1, b:2}, {a:2,b:3}], t.findOne( {_id:5} ).x );

t.save({ _id: 6, x: [ {a:{b:2}}, {a:{b:1}} ] } );
t.update( {_id:6}, { $push: { x: { $each: [ {a:{b:3}} ], $trimTo: 5, $sort: {'a.b':1} } } } )
assert.eq( [{a:{b:1}}, {a:{b:2}}, {a:{b:3}}], t.findOne( {_id:6} ).x );

t.save({ _id: 7, x: [ {a:{b:2}}, {a:{b:1}} ] } );
t.update( {_id:7}, { $push: { x: { $each: [ {a:{b:3}} ], $trimTo: 2, $sort: {'a.b':1} } } } )
assert.eq( [{a:{b:2}}, {a:{b:3}}], t.findOne( {_id:7} ).x );

t.save({ _id: 100, x: [ {a:1} ] } );
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [ 2 ], $trimTo:2, $sort: {a:1} } } } ) )
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2},1], $trimTo:2, $sort: {a:1} } } }))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $sort: {} } } } ) )
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:-2, $sort: {a:1} } } }))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2.1, $sort: {a:1} } } }))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $sort: {a:-2} } } }))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $sort: 1 } } } ) )
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $sort: {'a.':1} } }}))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $sort: {'':1} } } }))
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $trimTo:2, $xxx: {s:1} } } } ) )