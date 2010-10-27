// Move db between replica set shards -Tony

load('jstests/libs/grid.js')

function go() {

var N = 10000

// Create replica set of one server
var repset1 = new ReplicaSet('repset1', 1) .begin()
var conn1a = repset1.getMaster()
var db1a = conn1a.getDB('test')

// Add data to it
for (var i = 1; i <= N; i++) db1a['foo'].insert({x: i})

// Add another server to replica set
var conn1b = repset1.addServer()
conn1b.setSlaveOk()
var db1b = conn1b.getDB('test')

// Check that new server received replicated data
assert (db1b['foo'].count() == N, 'data did not replicate')

// Create sharding config servers
var configset = new ConfigSet(3)
configset.begin()

// Create sharding router (mongos)
var router = new Router(configset)
var routerConn = router.begin()
var db = routerConn.getDB('test')

// Add repset1 as only shard
addShard (routerConn, repset1.getURL())

// Add data via router and check it
db['foo'].update({}, {$set: {y: 'hello'}}, false, true)
assert (db['foo'].count({y: 'hello'}) == N,
    'updating and counting docs via router (mongos) failed')

// Create another replica set
var repset2 = new ReplicaSet('repset2', 2) .begin()
var conn2a = repset2.getMaster()

// Add repset2 as second shard
addShard (routerConn, repset2.getURL())

routerConn.getDB('admin').printShardingStatus()
printjson (conn2a.getDBs())

// Move test db from repset1 to repset2
moveDB (routerConn, 'test', repset2.getURL())

routerConn.getDB('admin').printShardingStatus()
printjson (conn2a.getDBs())

//Done
router.end()
configset.end()
repset2.stopSet()
repset1.stopSet()

print('moveprimary-replset.js SUCCESS')
}

go()
