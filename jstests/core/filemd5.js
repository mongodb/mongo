// Tests the basics of the filemd5 command.
// @tags: [
//     # Cannot implicitly shard accessed collections because of following error: GridFS fs.chunks
//     # collection must be sharded on either {files_id:1} or {files_id:1, n:1}
//     assumes_unsharded_collection,
//
//     # filemd5 command is not available on embedded
//     incompatible_with_embedded,
// ]

(function() {
    "use strict";

    db.fs.chunks.drop();
    assert.writeOK(db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "test")}));

    assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs"}), ErrorCodes.BadValue);

    db.fs.chunks.ensureIndex({files_id: 1, n: 1});
    assert.commandWorked(db.runCommand({filemd5: 1, root: "fs"}));

    assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs", partialOk: 1, md5state: 5}),
                                 50847);
    assert.writeOK(db.fs.chunks.insert({files_id: 2, n: 0}));
    assert.commandFailedWithCode(db.runCommand({filemd5: 2, root: "fs"}), 50848);
    assert.writeOK(db.fs.chunks.update({files_id: 2, n: 0}, {$set: {data: 5}}));
    assert.commandFailedWithCode(db.runCommand({filemd5: 2, root: "fs"}), 50849);
}());
