/**
 * When a minvalid timestamp `T` is set, applying a batch of operations earlier than `T` do not have
 * a consistent snapshot of data. In this case, we must write an invalidated document to the
 * `config.image_collection` for a retryable `findAndModify` that does not store images in the
 * oplog. This test ensures this behavior.
 *
 * To test this we:
 * -- Hold the stable timestamp back on a primary
 * -- Perform a number of retryable `findAndModify`s.
 * -- Manually set `minvalid` to a value inbetween what performed `findAndModify`s.
 * -- Restart the primary
 * -- Verify that operations after the `minvalid` timestamp are marked "valid" and those prior are
 *    marked "invalid". We set the `replBatchLimitOperations` to one to achieve this. This is
 *    necessary for the test due to manually setting minvalid. In production `minvalid` should
 *    always be unset, or set to the top of oplog.
 *
 * Because we restart the node, this only works on storage engines that persist data.
 *
 * @tags: [multiversion_incompatible, requires_majority_read_concern, requires_persistence]
 */

// Skip db hash check because replset cannot reach consistent state.
TestData.skipCheckDBHashes = true;

(function() {
"use strict";

let replTest = new ReplSetTest({name: "invalidate_images_when_minvalid", nodes: 1});

let nodes = replTest.startSet({
    setParameter: {
        featureFlagRetryableFindAndModify: true,
        storeFindAndModifyImagesInSideCollection: true,
        replBatchLimitOperations: 1
    }
});
replTest.initiate();
let primary = replTest.getPrimary();
let coll = primary.getDB("test")["invalidating"];
let images = primary.getDB("config")["image_collection"];
let minvalid = primary.getDB("local")["replset.minvalid"];

// Pause the WT stable timestamp to have a restart perform replication recovery on every operation
// in the test.
assert.commandWorked(primary.adminCommand({
    "configureFailPoint": 'WTPauseStableTimestamp',
    "mode": 'alwaysOn',
}));

function doRetryableFindAndModify(lsid, query, postImage, remove) {
    // Performs a retryable findAndModify. The document matched by query must exist. The
    // findAndModify will either be an update that sets the documents `updated: 1` or
    // removes the document. Returns the timestamp associated with the generated oplog entry.
    //
    // `postImage` and `remove` are booleans. The server rejects removes that ask for a post image.
    let cmd = {
        findandmodify: coll.getName(),
        lsid: {id: lsid},
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        query: query,
        new: postImage,
        upsert: false,
    };

    if (remove) {
        cmd["remove"] = true;
    } else {
        cmd["update"] = {$set: {updated: 1}};
    }

    return assert.commandWorked(coll.runCommand(cmd))["operationTime"];
}

// Each write contains arguments for calling `doRetryableFindAndModify`.
let invalidatedWrites = [
    {uuid: UUID(), query: {_id: 1}, postImage: false, remove: false},
    {uuid: UUID(), query: {_id: 2}, postImage: true, remove: false},
    {uuid: UUID(), query: {_id: 3}, postImage: false, remove: true}
];
let validWrites = [
    {uuid: UUID(), query: {_id: 4}, postImage: false, remove: false},
    {uuid: UUID(), query: {_id: 5}, postImage: true, remove: false},
    {uuid: UUID(), query: {_id: 6}, postImage: false, remove: true}
];

// Insert each document a query should match.
for (let idx = 0; idx < invalidatedWrites.length; ++idx) {
    assert.commandWorked(coll.insert(invalidatedWrites[idx]["query"]));
}
for (let idx = 0; idx < validWrites.length; ++idx) {
    assert.commandWorked(coll.insert(validWrites[idx]["query"]));
}

// Perform `findAndModify`s. Record the timestamp of the last `invalidatedWrites` to set minvalid
// with.
let lastInvalidatedImageTs = null;
for (let idx = 0; idx < invalidatedWrites.length; ++idx) {
    let write = invalidatedWrites[idx];
    lastInvalidatedImageTs = doRetryableFindAndModify(
        write['uuid'], write['query'], write['postImage'], write['remove']);
}
for (let idx = 0; idx < validWrites.length; ++idx) {
    let write = validWrites[idx];
    doRetryableFindAndModify(write['uuid'], write['query'], write['postImage'], write['remove']);
}

let imageDocs = [];
images.find().forEach((x) => {
    imageDocs.push(x);
});

jsTestLog({"MinValid": lastInvalidatedImageTs, "Pre-restart images": imageDocs});
assert.commandWorked(minvalid.update({}, {$set: {ts: lastInvalidatedImageTs}}));

replTest.restart(primary, undefined, true);

primary = replTest.getPrimary();
coll = primary.getDB("test")["invalidating"];
images = primary.getDB("config")["image_collection"];
minvalid = primary.getDB("local")["replset.minvalid"];

imageDocs = [];
images.find().forEach((x) => {
    imageDocs.push(x);
});
jsTestLog({"Post-restart images": imageDocs});

for (let idx = 0; idx < invalidatedWrites.length; ++idx) {
    let write = invalidatedWrites[idx];
    let image = images.findOne({"_id.id": write["uuid"]});

    assert.eq(1, image["txnNum"]);
    assert.eq(true, image["invalidated"]);
    assert.eq("minvalid suggests inconsistent snapshot", image["invalidatedReason"]);
    if (write["postImage"]) {
        assert.eq("postImage", image["imageKind"]);
    } else {
        assert.eq("preImage", image["imageKind"]);
    }
}

for (let idx = 0; idx < validWrites.length; ++idx) {
    let write = validWrites[idx];
    let image = images.findOne({"_id.id": write["uuid"]});

    assert.eq(1, image["txnNum"]);
    assert.eq(false, image["invalidated"]);
    if (write["postImage"]) {
        assert.eq("postImage", image["imageKind"]);

        let postImage = write["query"];
        postImage["updated"] = 1;
        assert.eq(postImage, image["image"]);
    } else {
        assert.eq("preImage", image["imageKind"]);
        assert.eq(write["query"], image["image"]);
    }
}

replTest.stopSet();
})();
