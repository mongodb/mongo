/**
 * If an incompatible index exists on a collection, the server should prevent updates to that index with non-fatal errors.
 * @tags: [
 *   # Older versions are expected to have fatal failures.
 *   requires_fcv_80
 * ]
 */

// Nonfatal error when attempting to update an improper timeseries-only index on a non-timeseries collection.

const collName = jsTestName();

const coll = db.getCollection(collName);
coll.drop();
assert.commandWorked(db.createCollection(collName));

// Prevents updating 2dsphere_bucket indices for top-level measurements.
// Authorization rules will normally prevent a non-system user from creating this index.
assert.commandWorked(coll.createIndex({x: "2dsphere_bucket"}));
assert.commandFailed(coll.insert({control: {version: 2}, x: HexData(0, "00")}));

// Prevents updating 2dsphere_bucket indices for nested measurements.
// Authorization rules will normally prevent a non-system user from creating this index.
assert.commandWorked(coll.createIndex({"data.a.b.c": "2dsphere_bucket"}));
assert.commandFailed(coll.insert({control: {version: 2}, data: {a: {b: {c: [0, 0]}}}}));

coll.drop();

