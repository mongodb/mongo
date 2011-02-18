/* NOTE: This test requires mongo shell to be built with V8 javascript engines so
fork() is available */

/*
1. Starts up a replica set with 2 servers and 1 arbiter, all with --dur option. 
2. Loads 10000 1K docs into a collection 
3. Forks 5 client threads, each $pushes then $pulls its own id to/from the same array in all document (multi-update) 
5. A 6th thread kills a random server in the replica set every 0-60 secs then restarts it 0-30 secs later. 
-Tony */

load('jstests/libs/fun.js')
load('jstests/libs/concurrent.js')

function random(n) {
    return Math.floor(Math.random() * n)
}

function makeText(size) {
    var text = ''
    for (var i = 0; i < size; i++) text += 'a'
    return text
}

function checkEqual (value, expected) {
    if (value != expected) throw ('expected ' + expected + ' got ' + value)
}

function deploy() {
    var rs = new ReplSetTest({nodes: 3, oplogSize: 1000})
    rs.startSet({dur: null})
    var cfg = rs.getReplSetConfig()
    cfg.members[2]['arbiterOnly'] = true
    rs.initiate(cfg)
    rs.awaitReplication()
    return rs
}

function confirmWrite(db) {
    var cmd = {getlasterror: 1, fsync: true, w: 2}
    var res = db.runCommand(cmd)
    if (! res.ok) throw (tojson(cmd) + 'failed: ' + tojson(res))
}

N = 10000
Text = makeText(1000)

function loadInitialData(rs) {
    var db = rs.getMaster().getDB('test')
    for (var i = 0; i < N; i++) db['col'].insert({x: i, text: Text})
    confirmWrite(db)
}

function newMasterConnection(ports) {
    for (var i = 0; i < ports.length; i++) {
      try {
       print ('Try connect to '+ i)
        var conn = new Mongo("127.0.0.1:" + ports[i])
        var rec = conn.getDB('admin').runCommand({ismaster: 1})
        if (rec && rec.ok && rec['ismaster']) {
         print ('Connected ' + i)
            return conn }
        // else close conn
      } catch(e) {}
    }
    throw 'no master: ' + ports
}

function rsMaster(ports, oldConn) {
    try {
        var rec = oldConn.getDB('admin').runCommand({ismaster: 1})
        if (rec['ismaster']) return oldConn
    } catch (e) {}
    return newMasterConnection(ports)
}

function queryAndUpdateData(ports) {return function(z) {
    var conn = null
    return function(i) {
      function printFailure(e) {print ('Q&U' + z + '-' + i + ': ' + e)}
      try {
        sleep(1000 + (z * 500))
        print('update ' + z + ' round ' + i)
        var db
        try {
            conn = rsMaster(ports, conn)
            db = conn.getDB('test')
        } catch (e) {
            printFailure(e)
            return
        }
        var n
        try {
            db['col'].update({}, {$push: {'z': z}}, false, true)
            n = db['col'].count({'z': z})
        } catch (e) {
            printFailure(e)
            return
        }
        checkEqual (n, N)
        sleep(1000)
        try {
            db['col'].update({}, {$pull: {'z': z}}, false, true)
            n = db['col'].count({'z': z})
        } catch (e) {
            printFailure(e)
            return
        }
        checkEqual (n, 0)
      } catch (e) {throw ('(Q&U' + z + '-' + i + ') ' + e)}
    }
}}

function killer(rs) {return function(i) {
  try {
    sleep(random(30) * 1000)
    var r = random(rs.ports.length - 1)
    print('Killing ' + r)
    stopMongod(rs.getPort(r), 9)  // hard kill
    sleep(random(30) * 1000)
    print('Restarting ' + r)
    rs.restart(r, {dur: null})
  } catch (e) {throw ('(Killer-' + i + ') ' + e)}
}}

function rsPorts(rs) {
    ports = new Array()
    for (var i = 0; i < rs.ports.length; i++) ports[i] = rs.getPort(i)
    return ports
}

function go(numRounds) {
    var rs = deploy()
    loadInitialData(rs)
    var jobs = map(queryAndUpdateData(rsPorts(rs)), [1,2,3,4,5])
    parallel (numRounds, jobs, [killer(rs)])
    sleep (2000)
    rs.stopSet()
    print("rsDurKillRestart1.js SUCCESS")
}
