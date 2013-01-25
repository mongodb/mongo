var st = new ShardingTest({ shards: 2, chunkSize: 1, other: { shardOptions: { verbose: 1 }} });
st.stopBalancer();

var testDB = st.s.getDB('test');
testDB.adminCommand({ enableSharding: 'test' });
testDB.adminCommand({ shardCollection: 'test.user', key: { x: 'hashed' }});

var configDB = st.s.getDB('config');
var chunkCountBefore = configDB.chunks.count();
assert.gt(chunkCountBefore, 1);

for (var x = 0; x < 1000; x++) {
    testDB.user.insert({ x: x });
}

// For debugging
(function() {
    var chunkList = configDB.chunks.find().sort({ min: -1 }).toArray();
    chunkList.forEach(function(chunk) { chunk.count = 0; });

    for (var x = 0; x < 1000; x++) {
        var hashVal = testDB.adminCommand({ _hashBSONElement: x }).out;
        var countSet = false;

        for (var y = 0; y < chunkList.length - 2; y++) {
            var chunkDoc = chunkList[y];
            if (chunkDoc.min.x <= hashVal) {
                countSet = true;
                chunkDoc.count++;

                print('doc in chunk: x [' + x + '], h[' + hashVal +
                      '], min[' + chunkDoc.min.x +
                      '], max[' + chunkDoc.max.x + ']');
                break;
            }
        }

        if (!countSet) {
            chunkDoc = chunkList[chunkList.length - 1];
            print('doc in chunk: x [' + x + '], h[' + hashVal +
                  '], min[' + chunkDoc.min.x +
                  '], max[' + chunkDoc.max.x + ']');
            chunkDoc.count++;
        }
    }

    chunkList.forEach(function(chunkDoc) {
        print('chunk details: ' + tojson(chunkDoc));
    });
});

var chunkDoc = configDB.chunks.find().sort({ min: 1 }).next();
var min = chunkDoc.min;
var max = chunkDoc.max;

// Assumption: There are documents in the MinKey chunk, otherwise, splitVector will
// fail. Note: This chunk will have 267 documents if collection was presplit to 4.
var cmdRes = testDB.adminCommand({ split: 'test.user', bounds: [ min, max ]});
assert(cmdRes.ok, 'split on bounds failed on chunk[' + tojson(chunkDoc) +
    ']: ' + tojson(cmdRes));

chunkDoc = configDB.chunks.find().sort({ min: 1 }).skip(1).next();
var middle = chunkDoc.min + 1000000;

cmdRes = testDB.adminCommand({ split: 'test.user', middle: { x: middle }});
assert(cmdRes.ok, 'split failed with middle [' + middle + ']: ' + tojson(cmdRes));

cmdRes = testDB.adminCommand({ split: 'test.user', find: { x: 7 }});
assert(cmdRes.ok, 'split failed with find: ' + tojson(cmdRes));

var chunkList = configDB.chunks.find().sort({ min: 1 }).toArray();
assert.eq(chunkCountBefore + 3, chunkList.length);

chunkList.forEach(function(chunkToMove) {
    var toShard = configDB.shards.findOne({ _id: { $ne: chunkToMove.shard }})._id;

    var cmdRes = testDB.adminCommand({ moveChunk: 'test.user',
        bounds: [ chunkToMove.min, chunkToMove.max ],
        to: toShard, _waitForDelete: true });
    assert(cmdRes.ok, 'Cmd failed: ' + tojson(cmdRes));
});

st.stop();

