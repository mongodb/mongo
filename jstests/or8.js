// missing collection

t = db.jstests_or8;
t.drop();

t.find({ "$or": [ { "PropA": { "$lt": "b" } }, { "PropA": { "$lt": "b", "$gt": "a" } } ] }).toArray(); 

// empty $in

t.save( {a:1} );
t.save( {a:3} );
t.ensureIndex( {a:1} );
t.find({ $or: [ { a: {$in:[]} } ] } ).toArray();
assert.eq.automsg( "2", "t.find({ $or: [ { a: {$in:[]} }, {a:1}, {a:3} ] } ).toArray().length" );
assert.eq.automsg( "2", "t.find({ $or: [ {a:1}, { a: {$in:[]} }, {a:3} ] } ).toArray().length" );
assert.eq.automsg( "2", "t.find({ $or: [ {a:1}, {a:3}, { a: {$in:[]} } ] } ).toArray().length" );
