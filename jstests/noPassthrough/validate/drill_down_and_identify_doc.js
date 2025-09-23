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

    // Unhash with 'prefix' should return 'differingDocId'.
    const res = assert.commandWorked(db1.coll.validate({collHash: true, unhash: [prefix]}));
    assert.eq(res.unhashed[prefix].length, 1, res);
    assert.eq(res.unhashed[prefix][0], {_id: differingDocId}, res);
}

hashDrillDown(db1, db2);

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
