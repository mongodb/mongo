// This test validates that map/reduce runs intermediate reduce steps in order to keep the
// in-memory state small. See SERVER-12949 for more details.
//
function assertGLEOK(status) {
    assert(status.ok && status.err === null, "Expected OK status object; found " + tojson(status));
}

var db = db.getSisterDB("MapReduceTestDB");
db.dropDatabase();

var coll = db.getCollection("mrInput");

// Insert 10 x 49 elements (10 i-s, 49 j-s)
//
var expectedOutColl = [];

var bulk = coll.initializeUnorderedBulkOp();
for (var i = 0; i < 10; i++) {
    for (var j = 1; j < 50; j++) {
        bulk.insert({idx: i, j: j});
    }
    expectedOutColl.push({_id: i, value: j - 1});
}
assert.writeOK(bulk.execute());

function mapFn() {
    emit(this.idx, 1);
}
function reduceFn(key, values) {
    return Array.sum(values);
}

var out = coll.mapReduce(mapFn, reduceFn, {out: {replace: "mrOutput"}});

// Check the output is as expected
//
var outColl = db.getCollection("mrOutput").find().toArray();
assert.eq(outColl, expectedOutColl, "The output collection is incorrect.");

assert.eq(out.counts.input, 490, "input count is wrong");
assert.eq(out.counts.emit, 490, "emit count is wrong");

// If this fails, most probably some of the configuration settings under mongo::mr::Config have
// changed, such as reduceTriggerRatio or maxInMemSize. If not the case, then something else
// must have changed with when intermediate reduces occur (see mongo::mr::State::checkSize).
//
assert.eq(out.counts.reduce, 14, "reduce count is wrong");
