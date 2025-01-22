// Cannot implicitly shard accessed collections because the coll.stats() output from a mongod when
// run against a sharded collection is wrapped in a "shards" object with keys for each shard.
// @tags: [
//   assumes_unsharded_collection,
//   not_allowed_with_signed_security_token,
// ]
//
// This test is designed to stress $sample, and any optimizations a storage engine might provide.
//
// A $sample stage as the first stage in a pipeline should ideally have a uniform distribution, so
// should at least have the following properties:
//   1. In a collection of N documents, we have a high probability of seeing at least N/4 distinct
//      documents after sampling N times.
//   2. We should not see any duplicate documents in any one $sample (this is only guaranteed if
//      there are no ongoing write operations).
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

let coll = db[jsTestName()];
coll.drop();

// If there is no collection, or no documents in the collection, we should not get any results
// from a sample.
assert.eq([], coll.aggregate([{$sample: {size: 1}}]).toArray());
assert.eq([], coll.aggregate([{$sample: {size: 10}}]).toArray());

db.createCollection(coll.getName());

// If we are performing secondary reads against a replica set, we need to wait for the created
// collection to replicate to all of the secondaries before we attempt to run coll.stats() on it
// since coll.stats() is not causally consistent.
if (FixtureHelpers.isReplSet(db)) {
    FixtureHelpers.awaitReplication(db);
}

// Test if we are running WT + LSM and if so, skip the test.
// WiredTiger LSM random cursor implementation doesn't currently give random enough
// distribution to pass this test case, so disable the test when checking an LSM
// configuration for now. We will need revisit this before releasing WiredTiger LSM
// as a supported file type. (See: WT-2403 for details on forthcoming changes)

const storageEngine = jsTest.options().storageEngine || "wiredTiger";

if (storageEngine === "wiredTiger" && coll.stats().wiredTiger.type === 'lsm') {
    quit();
}

assert.eq([], coll.aggregate([{$sample: {size: 1}}]).toArray());
assert.eq([], coll.aggregate([{$sample: {size: 10}}]).toArray());

// If there is only one document, we should get that document.
const paddingStr = "abcdefghijklmnopqrstuvwxyz";
const firstDoc = {
    _id: 0,
    paddingStr: paddingStr
};
assert.commandWorked(coll.insert(firstDoc));
assert.eq([firstDoc], coll.aggregate([{$sample: {size: 1}}]).toArray());
assert.eq([firstDoc], coll.aggregate([{$sample: {size: 10}}]).toArray());

// Insert a bunch of documents.
const bulk = coll.initializeUnorderedBulkOp();
const nDocs = 1000;
for (let id = 1; id < nDocs; id++) {
    bulk.insert({_id: id, paddingStr: paddingStr});
}
bulk.execute();

// Will contain a document's _id as a key if we've ever seen that document.
let cumulativeSeenIds = {};
const sampleSize = 10;

jsTestLog("About to do repeated samples, explain output: " +
          tojson(coll.explain().aggregate([{$sample: {size: sampleSize}}])));

// Repeatedly ask for small samples of documents to get a cumulative sample of size 'nDocs'.
for (let i = 0; i < nDocs / sampleSize; i++) {
    const results = coll.aggregate([{$sample: {size: sampleSize}}]).toArray();

    assert.eq(results.length, sampleSize, "$sample did not return the expected number of results");

    // Check that there are no duplicate documents in the result of any single sample.
    let idsThisSample = {};
    results.forEach(function recordId(result) {
        assert.lte(result._id, nDocs, "$sample returned an unknown document");
        assert(!idsThisSample[result._id],
               "A single $sample returned the same document twice: " + result._id);

        cumulativeSeenIds[result._id] = true;
        idsThisSample[result._id] = true;
    });
}

// An implementation would have to be very broken for this assertion to fail.
assert.gte(Object.keys(cumulativeSeenIds).length, nDocs / 4);

// Make sure we can return all documents in the collection.
assert.eq(coll.aggregate([{$sample: {size: nDocs}}]).toArray().length, nDocs);
