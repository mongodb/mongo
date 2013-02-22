// replica set as solo shard
// getLastError(2) fails on about every 170 inserts on my Macbook laptop -Tony
// TODO: Add assertion code that catches hang

load('jstests/libs/grid.js')

function go() {

    var N = 2000

    // ~1KB string
    var Text = ''
        for (var i = 0; i < 40; i++)
            Text += 'abcdefghijklmnopqrstuvwxyz'

    // Create replica set with 3 servers
    var repset1 = new ReplicaSet('repset1', 3) .begin()

    // Add data to it
    var conn1a = repset1.getMaster()
    var db1a = conn1a.getDB('test')
    for (var i = 0; i < N; i++) {
        db1a['foo'].insert({x: i, text: Text})
        db1a.getLastError(2)  // wait to be copied to at least one secondary
    }

    // Create 3 sharding config servers
    var configsetSpec = new ConfigSet(3)
    var configsetConns = configsetSpec.begin()

    // Create sharding router (mongos)
    var routerSpec = new Router(configsetSpec)
    var routerConn = routerSpec.begin()
    var dba = routerConn.getDB('admin')
    var db = routerConn.getDB('test')

    // Add repset1 as only shard
    addShard (routerConn, repset1.getURL())

    // Enable sharding on test db and its collection foo
    enableSharding (routerConn, 'test')
    db['foo'].ensureIndex({x: 1})
    shardCollection (routerConn, 'test', 'foo', {x: 1})

    sleep(30000)
    printjson (db['foo'].stats())
    dba.printShardingStatus()
    printjson (db['foo'].count())

    // Test case where GLE should return an error
    db.foo.insert({_id:'a', x:1});
    db.foo.insert({_id:'a', x:1});
    var x = db.getLastErrorObj(2, 30000)
    assert.neq(x.err, null, "C1 " + tojson(x));

    // Add more data
    for (var i = N; i < 2*N; i++) {
        db['foo'].insert({x: i, text: Text})
        var x = db.getLastErrorObj(2, 30000)  // wait to be copied to at least one secondary
            if (i % 30 == 0) print(i)
        if (i % 100 == 0 || x.err != null) printjson(x);
        assert.eq(x.err, null, "C2 " + tojson(x));
    }

    // take down the slave and make sure it fails over
    repset1.stop(1);
    repset1.stop(2);
    db.getMongo().adminCommand({setParameter: 1, logLevel:1});
    db.getMongo().setSlaveOk();
    print("trying some queries");
    assert.soon(function() { try {
                db.foo.find().next();
            }
            catch(e) {
                print(e);
                return false;
            }
            return true;
        }, "Queries took too long to complete correctly.",
        2 * 60 * 1000 );
    
    // Done
    routerSpec.end()
    configsetSpec.end()
    repset1.stopSet()

    print('shard_insert_getlasterror_w2.js SUCCESS')
}

//Uncomment below to execute
go()
