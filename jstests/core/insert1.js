t = db.insert1;
t.drop();

var o = {a: 1};
t.insert(o);
var doc = t.findOne();
assert.eq(1, doc.a);
assert(doc._id != null, tojson(doc));

t.drop();
o = {
    a: 2,
    _id: new ObjectId()
};
var id = o._id;
t.insert(o);
doc = t.findOne();
assert.eq(2, doc.a);
assert.eq(id, doc._id);

t.drop();
o = {
    a: 3,
    _id: "asdf"
};
id = o._id;
t.insert(o);
doc = t.findOne();
assert.eq(3, doc.a);
assert.eq(id, doc._id);

t.drop();
o = {
    a: 4,
    _id: null
};
t.insert(o);
doc = t.findOne();
assert.eq(4, doc.a);
assert.eq(null, doc._id, tojson(doc));

t.drop();
var toInsert = [];
var count = 100 * 1000;
for (i = 0; i < count; ++i) {
    toInsert.push({_id: i, a: 5});
}
assert.writeOK(t.insert(toInsert));
doc = t.findOne({_id: 1});
assert.eq(5, doc.a);
assert.eq(count, t.count(), "bad count");
