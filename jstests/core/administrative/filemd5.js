// Tests the basics of the filemd5 command.
// @tags: [
//     # Cannot implicitly shard accessed collections because of following error: GridFS fs.chunks
//     # collection must be sharded on either {files_id:1} or {files_id:1, n:1}
//     assumes_unsharded_collection,
//
// ]

db.fs.chunks.drop();
assert.commandWorked(db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "test")}));

assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs"}), ErrorCodes.NoQueryExecutionPlans);

db.fs.chunks.createIndex({files_id: 1, n: 1});
assert.soon(() => {
    const res = db.runCommand({filemd5: 1, root: "fs"});
    if (res.ok) {
        return true;
    }

    assert.commandFailedWithCode(res, ErrorCodes.NoQueryExecutionPlans);
    return false;
}, "filemd5 should succeed after index is created");

assert.commandFailedWithCode(db.runCommand({filemd5: 1, root: "fs", partialOk: 1, md5state: 5}), 50847);
assert.commandWorked(db.fs.chunks.insert({files_id: 2, n: 0}));
assert.commandFailedWithCode(db.runCommand({filemd5: 2, root: "fs"}), 50848);
assert.commandWorked(db.fs.chunks.update({files_id: 2, n: 0}, {$set: {data: 5}}));
assert.commandFailedWithCode(db.runCommand({filemd5: 2, root: "fs"}), 50849);

{
    const result = assert.commandWorked(
        db.runCommand({
            filemd5: ObjectId("000000000000000000000000"),
            root: "fs",
            partialOk: true,
        }),
    );

    assert(result.md5state !== undefined, "Expected md5state in filemd5 response with partialOk: true");
    const hex = result.md5state.hex();
    const totalBytes = hex.length / 2;
    assert.gt(totalBytes, 100, "md5state expected to be larger than 100 bytes");

    const checkFromByte = 100;
    const paddingHex = hex.substring(checkFromByte * 2);
    const allZeros = "0".repeat(paddingHex.length);
    assert.eq(paddingHex, allZeros, "md5state expected to be zero padded");
}
