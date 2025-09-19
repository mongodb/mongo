/**
 * Runs drill down to find the differing document.
 */

const conn1 = MongoRunner.runMongod();
const db1 = conn1.getDB("test");
const conn2 = MongoRunner.runMongod();
const db2 = conn2.getDB("test");

// We insert 100 documents (_id: 0 to _id: 99) on both nodes. However,
// on one node we add an extra field to a random document.
// Drill down needs to identify the differing document correctly.

for (let i = 0; i < 100; i++) {
    assert.commandWorked(db1.coll.insert({_id: i}));
    assert.commandWorked(db2.coll.insert({_id: i}));
}

Random.setRandomSeed();
const differingDocId = Random.randInt(100);
jsTest.log.info(`Updating doc on second node: {_id: ${differingDocId}}`);
assert.commandWorked(db2.coll.update({_id: differingDocId}, {$set: {a: "hello"}}));

// TODO (SERVER-109950): Currently we also insert the differing document in its own collection
// to know what hash bucket belongs to. This is to verify that the document found in hash
// drill down is indeed the one we modified.
// After unhash:true has been implemented, we can just run unhash:true instead and fetch the
// _id of the document.
assert.commandWorked(db1.verification.insert({_id: differingDocId}));
jsTest.log.info(
    "Verification collection partial: " +
        tojson(assert.commandWorked(db1.verification.validate({collHash: true, hashPrefixes: []})).partial),
);

// Keeps drilling down until the bucket size is 1, so that we are certain which bucket
// is differing.
function hashDrillDown(db1, db2) {
    // We start off by providing an empty string prefix, and from there we drill down
    // into each bucket.
    let prefix = "";
    let count = 2; // Start off with any value > 1.
    while (count > 1) {
        jsTest.log.info(`Drilling down with prefix: '${prefix}'`);
        let partial1 = assert.commandWorked(db1.coll.validate({collHash: true, hashPrefixes: [prefix]})).partial;
        let partial2 = assert.commandWorked(db2.coll.validate({collHash: true, hashPrefixes: [prefix]})).partial;
        jsTest.log.info("Partial1: " + tojson(partial1));
        jsTest.log.info("Partial2: " + tojson(partial2));
        assert.eq(Object.keys(partial1).length, Object.keys(partial2).length);
        const differingBuckets = [];
        for (const bucket in partial1) {
            if (bsonWoCompare(partial1[bucket], partial2[bucket]) !== 0) {
                differingBuckets.push(bucket);
            }
        }
        assert.eq(differingBuckets.length, 1, tojson(differingBuckets));
        prefix = differingBuckets[0];
        count = partial1[differingBuckets[0]].count;
    }

    jsTest.log.info("Prefix of differing bucket: " + tojson(prefix));

    // We expect the hash prefix we found via drill down to yield exactly one bucket in the
    // verification collection. The bucket is also expected to have exactly one document.
    const verifPartial = assert.commandWorked(
        db1.verification.validate({collHash: true, hashPrefixes: [prefix]}),
    ).partial;
    jsTest.log.info("Partial in verification collection: " + tojson(verifPartial));
    assert.eq(Object.keys(verifPartial).length, 1);
    const verifBucket = Object.keys(verifPartial)[0];
    assert.eq(verifPartial[verifBucket].count, 1);
}

hashDrillDown(db1, db2);

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
