/**
 * Runs drill down to find the differing document.
 */

const conn1 = MongoRunner.runMongod();
const db1 = conn1.getDB("test");
const conn2 = MongoRunner.runMongod();
const db2 = conn2.getDB("test");

Random.setRandomSeed();

// Tests drill down by drilling down until the bucket size is 1, so that we are certain which
// bucket is differing.
// 'multi:false' means one document will differ. 'multi:true' means two documents will differ.
function testDrillDown(multi) {
    jsTest.log.info("Testing with num differing docs: " + (multi ? 2 : 1));
    db1.coll.drop();
    db2.coll.drop();

    const numDocs = 100;
    // We insert numDocs documents (_id: 0 to _id: numDocs-1) on both nodes. However,
    // on one node we add an extra field to a random document.
    // Drill down needs to identify the differing document correctly.

    jsTest.log.info("Inserting documents...");
    const docs = [...Array(numDocs).keys()].map((id) => {
        return {_id: id};
    });
    assert.commandWorked(db1.coll.insertMany(docs));
    assert.commandWorked(db2.coll.insertMany(docs));

    let differingIds = [Random.randInt(numDocs)];
    if (multi) {
        differingIds.push(numDocs - 1 - differingIds[0]);
    }
    jsTest.log.info(`Updating docs with ids on second node: ${tojson(differingIds)}`);
    for (const id of differingIds) {
        assert.commandWorked(db2.coll.update({_id: id}, {$set: {a: "hello"}}));
    }

    let prefixes = [];
    let count = -1;
    do {
        jsTest.log.info(`Drilling down with prefixes: '${tojson(prefixes)}'`);
        let partial1 = assert.commandWorked(db1.coll.validate({collHash: true, hashPrefixes: prefixes})).partial;
        let partial2 = assert.commandWorked(db2.coll.validate({collHash: true, hashPrefixes: prefixes})).partial;
        jsTest.log.info("Partial1: " + tojson(partial1));
        jsTest.log.info("Partial2: " + tojson(partial2));
        assert.eq(Object.keys(partial1).length, Object.keys(partial2).length);
        const differingBuckets = [];
        for (const bucket in partial1) {
            if (bsonWoCompare(partial1[bucket], partial2[bucket]) !== 0) {
                differingBuckets.push(bucket);
                count = Math.max(partial1[bucket].count, count);
            }
        }
        assert.lte(differingBuckets.length, 2, tojson(differingBuckets));
        prefixes = differingBuckets;
    } while (count > 1);

    jsTest.log.info("Prefixes of differing buckets: " + tojson(prefixes));

    // revealHashedIds with 'prefix' should return 'differingIds'.
    const revealedIds = assert.commandWorked(
        db1.coll.validate({collHash: true, revealHashedIds: prefixes}),
    ).revealedIds;
    jsTest.log.info("Revealed ids: " + tojson(revealedIds));
    const ids = Object.keys(revealedIds).map((prefix) => {
        return revealedIds[prefix][0]["_id"];
    });
    assert.eq(ids.length, multi ? 2 : 1);
    assert.sameMembers(ids, differingIds, ids);
}

testDrillDown(/*multi=*/ false);
testDrillDown(/*multi=*/ true);

MongoRunner.stopMongod(conn1);
MongoRunner.stopMongod(conn2);
