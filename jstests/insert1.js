t = db.insert1;
t.drop();

o = {a:1};
t.insert(o);
id = t._lastID
assert.eq(o, {a:1}, "input unchanged 1");
assert.eq(typeof(id), "object", "1");
assert.eq(id.constructor, ObjectId, "1");
assert.eq(t.findOne({_id:id}).a, 1, "find by id 1");
assert.eq(t.findOne({a:1})._id, id , "find by val 1");

o = {a:2, _id:new ObjectId()};
id1 = o._id
t.insert(o);
id2 = t._lastID
assert.eq(id1, id2, "ids match 2");
assert.eq(o, {a:2, _id:id1}, "input unchanged 2");
assert.eq(typeof(id2), "object", "2");
assert.eq(id2.constructor, ObjectId, "2");
assert.eq(t.findOne({_id:id1}).a, 2, "find by id 2");
assert.eq(t.findOne({a:2})._id, id1 , "find by val 2");

o = {a:3, _id:"asdf"};
id1 = o._id
t.insert(o);
id2 = t._lastID
assert.eq(id1, id2, "ids match 3");
assert.eq(o, {a:3, _id:id1}, "input unchanged 3");
assert.eq(typeof(id2), "string", "3");
assert.eq(t.findOne({_id:id1}).a, 3, "find by id 3");
assert.eq(t.findOne({a:3})._id, id1 , "find by val 3");

o = {a:4, _id:null};
id1 = o._id
t.insert(o);
id2 = t._lastID
assert.eq(id1, id2, "ids match 4");
assert.eq(o, {a:4, _id:id1}, "input unchanged 4");
assert.eq(t.findOne({_id:id1}).a, 4, "find by id 4");
assert.eq(t.findOne({a:4})._id, id1 , "find by val 4");

var stats = db.runCommand({ collstats: "insert1" });
assert(stats.paddingFactor == 1.0);
