var shardOpts = [
    {noscripting: ''},
    {}  // just use default params
];

var st = new ShardingTest({shards: shardOpts, other: {nopreallocj: 1}});
var mongos = st.s;

st.shardColl('bar', {x: 1});

var testDB = mongos.getDB('test');
var coll = testDB.bar;

coll.insert({x: 1});

var map = function() {
    emit(this.x, 1);
};

var reduce = function(key, values) {
    return 1;
};

var mrResult = testDB.runCommand({mapreduce: 'bar', map: map, reduce: reduce, out: {inline: 1}});

assert.eq(0, mrResult.ok, 'mr result: ' + tojson(mrResult));

// Confirm that mongos did not crash
assert(testDB.adminCommand({serverStatus: 1}).ok);

// Confirm that the rest of the shards did not crash
mongos.getDB('config').shards.find().forEach(function(shardDoc) {
    var shardConn = new Mongo(shardDoc.host);
    var adminDB = shardConn.getDB('admin');
    var cmdResult = adminDB.runCommand({serverStatus: 1});

    assert(cmdResult.ok,
           'serverStatus on ' + shardDoc.host + ' failed, result: ' + tojson(cmdResult));
});
