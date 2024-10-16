/**
 * Tests inserting large documents into a capped collection.
 *
 * @tags: [
 *     # Suites with stepdowns will result in replication rollback, and we do not update tracked
 *     # collection size for replication rollback. Thus if this occurs during the inserts, we can
 *     # end up with fewer documents in the capped collection than we would otherwise expect.
 *     does_not_support_stepdowns,
 *     requires_capped,
 *     requires_collstats,
 *     requires_fastcount,
 *     # Capped collections cannot be sharded
 *     assumes_unsharded_collection,
 * ]
 */
const coll = db.capped_large_docs;
coll.drop();

const maxSize = 25 * 1024 * 1024;  // 25MB.
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: maxSize}));

// Insert ~50MB of data.
const doc = {
    key: "a".repeat(10 * 1024 * 1024)
};
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert(doc));
}

// With a capped collection capacity of 25MB, we should have 2 documents.
const stats = assert.commandWorked(coll.stats());
assert.eq(2, stats.count);
assert(stats.size <= maxSize);