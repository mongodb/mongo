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
 *   expects_explicit_underscore_id_index,
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
assert.eq(catalog[0].md.indexes.length, 2);
assert.eq(catalog[0].md.indexes[0].spec.name, "_id_");
assert.eq(catalog[0].md.indexes[1].spec.sparse, nonBooleanValue);

// Verify that getIndexes() converts the value to a boolean.
const indexes = t.getIndexes();
assert.eq(indexes.length, 2);
assert.eq(indexes[0].name, "_id_");
assert.eq(indexes[1].sparse, true);
