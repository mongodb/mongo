// missing collection

t = db.jstests_or8;
t.drop();

t.find({ "$or": [ { "PropA": { "$lt": "b" } }, { "PropA": { "$lt": "b", "$gt": "a" } } ] }).toArray(); 

// empty $in

//t.ensureIndex( {a:1} );
//t.find({ $or: [ { a: {$in:[]} } ] } ).toArray();
