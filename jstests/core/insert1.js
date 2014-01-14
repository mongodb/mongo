t = db.insert1;
t.drop();

var o = {a:1};
t.insert(o);
var doc = t.findOne();
assert.eq(1, doc.a);
assert(doc._id != null, tojson(doc));

t.drop();
o = {a:2, _id:new ObjectId()};
var id = o._id;
t.insert(o);
doc = t.findOne();
assert.eq(2, doc.a);
assert.eq(id, doc._id);

t.drop();
o = {a:3, _id:"asdf"};
id = o._id;
t.insert(o);
doc = t.findOne();
assert.eq(3, doc.a);
assert.eq(id, doc._id);

t.drop();
o = {a:4, _id:null};
t.insert(o);
doc = t.findOne();
assert.eq(4, doc.a);
assert.eq(null, doc._id, tojson(doc));

var stats = db.runCommand({ collstats: "insert1" });
assert(stats.paddingFactor == 1.0);
