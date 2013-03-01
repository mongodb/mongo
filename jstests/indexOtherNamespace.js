// SERVER-8814: Test that only the system.indexes namespace can be used to build indexes.

function assertGLENotOK(status) {
    assert(status.ok && status.err !== null,
           "Expected not-OK status object; found " + tojson(status));
}

db = db.getSiblingDB("indexOtherNS");
db.dropDatabase();

db.foo.insert({a:1})
assert.eq(1, db.system.indexes.count());
assert.eq("BasicCursor", db.foo.find({a:1}).explain().cursor);

db.randomNS.system.indexes.insert({ns:"indexOtherNS.foo", key:{a:1}, name:"a_1"});
assertGLENotOK(db.getLastErrorObj());
// Assert that index didn't actually get built
assert.eq(1, db.system.indexes.count());
assert.eq(null, db.system.namespaces.findOne({name : "indexOtherNS.foo.$a_1"}));
assert.eq("BasicCursor", db.foo.find({a:1}).explain().cursor);
