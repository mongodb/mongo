/**
 * Runs drill down on just one node with many documents. Rather than having a second node
 * to compare against, we use a reference collection with a single document.
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("test");

// Insert numDocs documents on the node.
const numDocs = 100000;
const docs = [...Array(numDocs).keys()].map((id) => {
    return {_id: id};
});
jsTest.log.info(`Inserting ${numDocs} documents`);
assert.commandWorked(db.coll.insertMany(docs));
jsTest.log.info(`Done inserting documents`);

// Insert the document in the reference collection so that we know what its hash prefix
// should be.
Random.setRandomSeed();
const differingDocId = Random.randInt(numDocs);
jsTest.log.info(`Inserting doc in reference collection: {_id: ${differingDocId}}`);
assert.commandWorked(db.ref_coll.insert({_id: differingDocId}));

let prefix = [];
let count;

do {
    jsTest.log.info(`Drilling down with prefix: '${prefix}'`);
    // Get the prefix of the document in the reference collection.
    let refPartial = assert.commandWorked(db.ref_coll.validate({collHash: true, hashPrefixes: prefix})).partial;
    let refPrefix = Object.keys(refPartial)[0];

    jsTest.log.info(`Run validate on the collection, and ensure that bucket '${refPrefix}' exists.`);
    let partial = assert.commandWorked(db.coll.validate({collHash: true, hashPrefixes: prefix})).partial;
    assert(partial[refPrefix], partial);
    assert.gt(partial[refPrefix].count, 0);

    count = partial[refPrefix].count;
    prefix = [refPrefix];
} while (count > 1);

// When we reveal the _id with "prefix" we should get just the document we had modified.
const res = assert.commandWorked(db.coll.validate({collHash: true, revealHashedIds: prefix}));
assert.eq(res.revealedIds[prefix[0]].length, 1, res);
assert.eq(res.revealedIds[prefix[0]][0], {_id: differingDocId}, res);

MongoRunner.stopMongod(conn);
