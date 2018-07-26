// Tests the behavior of an $out stage which encounters an error in the middle of processing. We
// don't guarantee any particular behavior in this scenario, but this test exists to make sure
// nothing horrendous happens and to characterize the current behavior.
// @tags: [assumes_unsharded_collection]
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // For assertErrorCode.

    const coll = db.batch_writes;
    const outColl = db.batch_writes_out;
    coll.drop();
    outColl.drop();

    // Test with 2 very large documents that do not fit into a single batch.
    const kSize15MB = 15 * 1024 * 1024;
    const largeArray = new Array(kSize15MB).join("a");
    assert.commandWorked(coll.insert({_id: 0, a: largeArray}));
    assert.commandWorked(coll.insert({_id: 1, a: largeArray}));

    // Make sure the $out succeeds without any duplicate keys.
    ["replaceCollection", "insertDocuments", "replaceDocuments"].forEach(mode => {
        coll.aggregate([{$out: {to: outColl.getName(), mode: mode}}]);
        assert.eq(2, outColl.find().itcount());
        outColl.drop();
    });

    coll.drop();
    for (let i = 0; i < 10; i++) {
        assert.commandWorked(coll.insert({_id: i, a: i}));
    }

    // Create a unique index on 'a' in the output collection to create a unique key violation when
    // running the $out. The second document to be written ({_id: 1, a: 1}) will conflict with the
    // existing document in the output collection. We use a unique index on a field other than _id
    // because "replaceDocuments" mode will not change _id when one already exists.
    outColl.drop();
    assert.commandWorked(outColl.insert({_id: 2, a: 1}));
    assert.commandWorked(outColl.createIndex({a: 1}, {unique: true}));

    // Test that both batched updates and inserts will successfully write the first document but
    // fail on the second. We don't guarantee any particular behavior in this case, but this test is
    // meant to characterize the current behavior.
    assertErrorCode(coll, [{$out: {to: outColl.getName(), mode: "insertDocuments"}}], 16996);
    assert.soon(() => {
        return outColl.find().itcount() == 2;
    });

    assertErrorCode(coll, [{$out: {to: outColl.getName(), mode: "replaceDocuments"}}], 50904);
    assert.soon(() => {
        return outColl.find().itcount() == 2;
    });

    // Mode "replaceCollection" will drop the contents of the output collection, so there is no
    // duplicate key error.
    outColl.drop();
    assert.commandWorked(outColl.insert({_id: 2, a: 1}));
    assert.commandWorked(outColl.createIndex({a: 1}, {unique: true}));
    coll.aggregate([{$out: {to: outColl.getName(), mode: "replaceCollection"}}]);
    assert.eq(10, outColl.find().itcount());
}());
