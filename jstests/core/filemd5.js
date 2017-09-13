// Cannot implicitly shard accessed collections because of following error: GridFS fs.chunks
// collection must be sharded on either {files_id:1} or {files_id:1, n:1}
// @tags: [assumes_unsharded_collection]

db.fs.chunks.drop();
db.fs.chunks.insert({files_id: 1, n: 0, data: new BinData(0, "test")});

x = db.runCommand({"filemd5": 1, "root": "fs"});
assert(!x.ok, tojson(x));

db.fs.chunks.ensureIndex({files_id: 1, n: 1});
x = db.runCommand({"filemd5": 1, "root": "fs"});
assert(x.ok, tojson(x));
