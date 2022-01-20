// @tags: [requires_getmore]

(function() {
'use strict';

load("jstests/libs/fixture_helpers.js");

var coll = db.exhaustColl;
coll.drop();

const docCount = 4;
for (var i = 0; i < docCount; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Check that the query works without exhaust set
assert.eq(coll.find().batchSize(1).itcount(), docCount);

// Now try to run the same query with exhaust
try {
    assert.eq(coll.find().batchSize(1).addOption(DBQuery.Option.exhaust).itcount(), docCount);
} catch (e) {
    // The exhaust option is not valid against mongos, ensure that this query throws the right
    // code
    assert.eq(e.code, 18526, () => tojson(e));
}

// Test a case where the amount of data requires a response to the initial find operation as well as
// three getMore reply batches.
(function() {
// Skip this test case if we are connected to a mongos, since exhaust queries generally aren't
// expected to work against a mongos.
if (FixtureHelpers.isMongos(db)) {
    return;
}

coll.drop();

// Include a long string in each document so that the documents are a bit bigger than 16KB.
const strSize = 16 * 1024;
// The docs are ~16KB and each getMore response is 16MB. Therefore, a full getMore batch will
// contain about 1000 documents. Since the initial find response is limited to only 101 documents,
// by inserting 3000 we ensure that three subsequent getMore replies are required. Roughly speaking,
// the initial reply will consist of the first 100, the first getMore reply 1000 more, then another
// 1000, and then the remaining 900.
const numDocs = 3000;

const str = "A".repeat(strSize);

let bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({key: str});
}
assert.commandWorked(bulk.execute());

assert.eq(numDocs, coll.find().addOption(DBQuery.Option.exhaust).itcount());
}());
}());
