// Cannot implicitly shard accessed collections because of extra shard key index in sharded
// collection.
// @tags: [
//   assumes_no_implicit_index_creation,
//   requires_fastcount,
// ]

let t = db.geo1;
t.drop();

let idx = {loc: "2d", zip: 1};

t.insert({zip: "06525", loc: [41.352964, 73.01212]});
t.insert({zip: "10024", loc: [40.786387, 73.97709]});
assert.commandWorked(t.insert({zip: "94061", loc: [37.463911, 122.23396]}));

// test "2d" has to be first
assert.eq(1, t.getIndexKeys().length, "S1");
t.createIndex({zip: 1, loc: "2d"});
assert.eq(1, t.getIndexKeys().length, "S2");

t.createIndex(idx);
assert.eq(2, t.getIndexKeys().length, "S3");

assert.eq(3, t.count(), "B1");
assert.writeError(t.insert({loc: [200, 200]}));
assert.eq(3, t.count(), "B3");

// test normal access

let wb = t.findOne({zip: "06525"});
assert(wb, "C1");

assert.eq("06525", t.find({loc: wb.loc}).hint({"$natural": 1})[0].zip, "C2");
assert.eq("06525", t.find({loc: wb.loc})[0].zip, "C3");
// assert.eq( 1 , t.find( { loc : wb.loc } ).explain().nscanned , "C4" )

// test config options

t.drop();

t.createIndex({loc: "2d"}, {min: -500, max: 500, bits: 4});
assert.commandWorked(t.insert({loc: [200, 200]}));
