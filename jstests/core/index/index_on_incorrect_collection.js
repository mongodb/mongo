/**
 * If an incompatible index exists on a collection, the server should prevent updates to that index
 * with non-fatal errors.
 */

// Nonfatal error when attempting to update an improper timeseries-only index on a non-timeseries
// collection.
const collName = jsTestName();
let coll = db.getCollection(collName);
coll.drop();

const before = function() {
    assert.commandWorked(db.createCollection(collName));
    coll = db.getCollection(collName);
};
const after = function() {
    coll.drop();
};

// Prevents updating 2dsphere_bucket indices for top-level measurements.
before();
{
    const indexSpec = {x: "2dsphere_bucket"};
    // TODO SERVER-118911 index creation should not be possible
    assert.commandWorked(coll.createIndex(indexSpec));
    const sampleDoc = {control: {version: 2}, x: HexData(0, "00")};
    assert.commandFailed(coll.insert(sampleDoc));
    // Ensure the index can be dropped, allowing the sampleDoc to be inserted
    const res = assert.commandWorked(coll.dropIndex(indexSpec));
    assert.commandWorked(coll.insert(sampleDoc));
}
after();

// Prevents updating 2dsphere_bucket indices for nested measurements.
before();
{
    const indexSpec = {"data.a.b.c": "2dsphere_bucket"};
    // TODO SERVER-118911 index creation should not be possible
    assert.commandWorked(coll.createIndex(indexSpec));
    const sampleDoc = {control: {version: 2}, data: {a: {b: {c: [0, 0]}}}};
    assert.commandFailed(coll.insert(sampleDoc));
    // Ensure the index can be dropped, allowing the sampleDoc to be inserted
    const res = assert.commandWorked(coll.dropIndex(indexSpec));
    assert.commandWorked(coll.insert(sampleDoc));
}
after();

// Cannot build a 2dsphere_bucket index on a populated collection
before();
{
    assert.commandWorked(coll.insert({abc: "xyz"}));
    assert.commandFailed(coll.createIndex({x: "2dsphere_bucket"}));
    assert.commandFailed(coll.createIndex({"data.a.b.c": "2dsphere_bucket"}));
}
after();
