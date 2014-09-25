// tests gridfs with a sharded fs.chunks collection.

var test = new ShardingTest({shards: 3,
                             mongos: 1,
                             config: 1,
                             verbose: 2,
                             other: {chunksize:1, separateConfig:true}})

var mongos = test.s0


var filename = "mongod" // A large file we are guaranteed to have
if (_isWindows())
    filename += ".exe"

function testGridFS(name) {
    var d = mongos.getDB(name)

    // this function should be called on a clean db
    assert.eq(d.name.files.count(), 0)
    assert.eq(d.fs.chunks.count(), 0)

    var rawmd5 = md5sumFile(filename)

    // upload file (currently calls filemd5 internally)
    runMongoProgram.apply(null, ["mongofiles", "--port", mongos.port, "put", filename, '--db', name]);

    assert.eq(d.fs.files.count(), 1)
    var fileObj = d.fs.files.findOne()
    print("fileObj: " + tojson(fileObj))
    assert.eq(rawmd5, fileObj.md5) //check that mongofiles inserted the correct md5

    // Call filemd5 ourself and check results.
    var res = d.runCommand({filemd5: fileObj._id})
    print("filemd5 output: " + tojson(res))
    assert(res.ok)
    assert.eq(rawmd5, res.md5)

    var numChunks = d.fs.chunks.find({files_id: fileObj._id}).itcount()
    //var numChunks = d.fs.chunks.count({files_id: fileObj._id}) // this is broken for now
    assert.eq(numChunks, res.numChunks)
}

print('\n\n\t**** unsharded ****\n\n')
name = 'unsharded'
testGridFS(name)

print('\n\n\t**** sharded db, unsharded collection ****\n\n')
name = 'sharded_db'
test.adminCommand({enablesharding: name})
testGridFS(name)

print('\n\n\t**** sharded collection on files_id ****\n\n')
name = 'sharded_files_id'
test.adminCommand({enablesharding: name})
test.adminCommand({shardcollection: name+'.fs.chunks', key: {files_id:1}})
testGridFS(name)

print('\n\n\t**** sharded collection on files_id,n ****\n\n')
name = 'sharded_files_id_n'
test.adminCommand({enablesharding: name})
test.adminCommand({shardcollection: name+'.fs.chunks', key: {files_id:1, n:1}})
testGridFS(name)

test.stop()
