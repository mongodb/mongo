// BUG: [SERVER-1768] replica set getlasterror {w: 2} after 2000 
// inserts hangs while secondary servers log "replSet error RS102 too stale to catch up" every once in a while

function newReplicaSet (name, numServers) {
    var rs = new ReplSetTest({name: name, nodes: numServers})
    rs.startSet()
    rs.initiate()
    rs.awaitReplication()
    return rs
}

function go() {
var N = 2000

// ~1KB string
var Text = ''
for (var i = 0; i < 40; i++)
    Text += 'abcdefghijklmnopqrstuvwxyz'

// Create replica set of 3 servers
var repset = newReplicaSet('repset', 3)
var conn = repset.getMaster()
var db = conn.getDB('test')

// Add data to it
for (var i = 0; i < N; i++)
    db['foo'].insert({x: i, text: Text})

// wait to be copied to at least one secondary (BUG hangs here)
db.getLastError(2)

print('getlasterror_w2.js SUCCESS')
}

// turn off until fixed 
//go();
