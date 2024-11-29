/**
 * Tests that creating an index with a non-trivial integer value in the `sparse` field works.
 * It is equivalent to creating a normal sparse index, and the value persists in the catalog.
 *
 * @tags: [
 *   # Cannot implicitly shard accessed collections because of extra shard key index in sharded
 *   # collection.
 *   assumes_no_implicit_index_creation,
 *   # $listCatalog not supported inside of a multi-document transaction.
 *   does_not_support_transactions,
 *   # $listCatalog does not include the tenant prefix in its results.
 *   command_not_supported_in_serverless,
 *   requires_fastcount
 * ]
 */

let t = db.index_sparse_create_index_non_boolean_value;
t.drop();

const nonBooleanValue = 12345;

t.insert({_id: 1, x: 1});
t.insert({_id: 2, x: 2});
t.insert({_id: 3, x: 2});
t.insert({_id: 4});
t.insert({_id: 5});

t.createIndex({x: 1}, {sparse: nonBooleanValue});
assert.eq(2, t.getIndexes().length);
assert.eq(5, t.find().sort({x: 1}).itcount());

// Verify that the original value has been persisted in the catalog.
const catalog = t.aggregate([{$listCatalog: {}}]).toArray();
assert.eq(catalog[0].db, db.getName());
assert.eq(catalog[0].name, t.getName());
const sparseIndexInCatalog = catalog[0].md.indexes.find(i => i.spec.name === "x_1");
assert(sparseIndexInCatalog, "Could not find x_1 index in: " + tojson(catalog[0].md.indexes));
assert.eq(sparseIndexInCatalog.spec.sparse, nonBooleanValue);

// Verify that getIndexes() converts the value to a boolean.
const indexes = t.getIndexes();
const sparseIndexInIndexes = indexes.find(i => i.name === "x_1");
assert(sparseIndexInIndexes, "Could not find x_1 index in: " + tojson(indexes));
assert.eq(sparseIndexInIndexes.sparse, true);
