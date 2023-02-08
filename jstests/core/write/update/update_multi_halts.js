/**
 * When doing multi-update, we need to ensure that even if a record shows up in the scan multiple
 * times, we only update it once. Note: these tests don't verify the correctness of specific update
 * operators as it's done elsewhere.
 *
 * @tags: [
 *   requires_multi_updates,
 *   requires_non_retryable_writes,
 *   # 'upsert' requires a filter that includes the shard key, which is '_id' but for 'testUpsert()'
 *   # we need to ensure that either collscan or ixscan of a specific index are used.
 *   assumes_unsharded_collection
 * ]
 */

(function() {
"use strict";

let coll = db.update_multi_halts;

function testUpsert(withIndex) {
    coll.drop();

    // Insert something into the collection, as update on empty collections might be optimized.
    assert.commandWorked(coll.insertMany([{key: 0, cUpdates: 0}]));
    if (withIndex) {
        assert.commandWorked(coll.createIndex({key: 1}));
    }

    let options = {upsert: true};
    if (withIndex) {
        // Make sure the index is used.
        options.hint = {key: 1};
    }

    // The upserted record matches the filter but shouldn't be re-updated.
    assert.commandWorked(
        coll.updateMany({key: {$gt: 0}}, {$set: {key: 1}, $inc: {cUpdates: 1}}, options));

    assert.eq(2, coll.count(), "Count of documents in the collection after upsert");
    assert.eq(1, coll.count({key: 1}), "Count of documents with the new key");
    assert.eq(1, coll.find({key: 1}).toArray()[0].cUpdates, "Number of updates on the target doc");
}
testUpsert(false /* use collscan for the filter */);
testUpsert(true /* use ixscan for the filter */);

(function testCollscanNotInplaceUpdate() {
    coll.drop();

    assert.commandWorked(coll.insert([{key: 1, cUpdates: 0}]));

    // The record that cannot be updated in-place (because adding a new field) should not show up
    // again during the scan.
    assert.commandWorked(coll.updateMany({key: 1}, {$set: {new: 42}, $inc: {cUpdates: 1}}));

    assert.eq(1, coll.count(), "Count of documents in the collection after update");
    assert.eq(1, coll.find().toArray()[0].cUpdates, "Number of updates on the target doc");
})();

(function testIxscanUpdate() {
    coll.drop();

    coll.insert({key: 0, cUpdates: 0});
    coll.createIndex({key: 1});

    // The update of 'key' pushes the updated record "down" the index so it will reappear in the
    // index scan.
    assert.commandWorked(coll.updateMany({key: {$gte: 0}},                // filter
                                         {$inc: {key: 10, cUpdates: 1}},  // update
                                         {hint: {key: 1}}));              // options

    assert.eq(1, coll.count(), "Count of documents in the collection after update");
    assert.eq(1, coll.find().toArray()[0].cUpdates, "Number of updates on the target doc");
})();
})();
