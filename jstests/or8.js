// missing collection

t = db.jstests_or8;
t.drop();

t.find({ "$or": [ { "PropA": { "$lt": "b" } }, { "PropA": { "$lt": "b", "$gt": "a" } } ] }).toArray(); 
