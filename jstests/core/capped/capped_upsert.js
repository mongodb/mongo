/**
 * Tests upserting into a capped collection with deletes needed.
 *
 * @tags: [
 *     requires_capped,
 *     # Capped collections cannot be sharded
 *     assumes_unsharded_collection,
 * ]
 */
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 100000, max: 1}));
assert.commandWorked(coll.insert({text: "a"}));
assert.commandWorked(coll.save({_id: 123, text: "b"}));
