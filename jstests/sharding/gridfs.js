// tests gridfs with a sharded fs.chunks collection.

var test = new ShardingTest({shards: 3, mongos: 1, config: 1, other: {chunksize:1, separateConfig:true}})

var mongos = test.s0

var d = mongos.getDB("test")

var filename = "mongod" // A large file we are guaranteed to have

function reset() {
    d.fs.files.drop()
    d.fs.chunks.drop()
}

function testGridFS() {
    // this function should be called on a clean db
    assert.eq(d.fs.files.count(), 0)
    assert.eq(d.fs.chunks.count(), 0)

    var rawmd5 = md5sumFile(filename)

    // upload file (currently calls filemd5 internally)
    runMongoProgram("mongofiles", "--port", mongos.port, "put", filename)

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
testGridFS()
reset()

print('\n\n\t**** sharded db, unsharded collection ****\n\n')
test.adminCommand({enablesharding: 'test'})
testGridFS()
reset()

print('\n\n\t**** sharded collection on files_id ****\n\n')
test.adminCommand({shardcollection: 'test.fs.chunks', key: {files_id:1}})
testGridFS()
reset()

print('\n\n\t**** sharded collection on files_id,n ****\n\n')
test.adminCommand({shardcollection: 'test.fs.chunks', key: {files_id:1, n:1}})
testGridFS()
reset()

test.stop()
