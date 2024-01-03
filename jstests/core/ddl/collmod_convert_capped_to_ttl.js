/**
 * Test for the collMod command to convert an index to a TTL index on a capped collection. Separate
 * from collmod_convert_to_ttl.js to resolve FCV upgrade downgrade related race condition.
 * TODO: SERVER-79318 merge this back into collmod_convert_to_ttl.js when
 * TTLIndexesOnCappedCollections is removed.
 *
 * @tags: [
 *  # Capped collections cannot be sharded.
 *  assumes_unsharded_collection,
 *  requires_ttl_index,
 *  requires_fcv_71,
 * ]
 */

'use strict';

const collName = jsTestName();

function findTTL(collection, key, expireAfterSeconds) {
    const all = collection.getIndexes().filter(function(z) {
        return z.expireAfterSeconds == expireAfterSeconds && friendlyEqual(z.key, key);
    });
    return all.length === 1;
}

const collCapped = db.getCollection(collName + "_capped");
collCapped.drop();
db.createCollection(collCapped.getName(), {capped: true, size: 1024 * 1024});
collCapped.createIndex({a: 1});

assert.commandWorked(db.runCommand({
    "collMod": collCapped.getName(),
    "index": {"keyPattern": {a: 1}, "expireAfterSeconds": 100},
}));
assert(findTTL(collCapped, {a: 1}, 100), "TTL index should be 100 now");
