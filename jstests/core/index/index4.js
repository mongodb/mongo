// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
// ]

// index4.js

let t = db.index4;
t.drop();

t.save({name: "alleyinsider", instances: [{pool: "prod1"}, {pool: "dev1"}]});

t.save({name: "clusterstock", instances: [{pool: "dev1"}]});

// this should fail, not allowed -- we confirm that.
t.createIndex({instances: {pool: 1}});
assert.eq(1, t.getIndexes().length, "no indexes other than _id should be here yet");

t.createIndex({"instances.pool": 1});

sleep(10);

let a = t.find({instances: {pool: "prod1"}});
assert(a.length() == 1, "len1");
assert(a[0].name == "alleyinsider", "alley");

assert(t.validate().valid, "valid");
