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
    var rs = new ReplSetTest({nodes: 3, oplogSize: 40})
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

function rsMaster(rs) {
    var prevMaster = rs.liveNodes.master
    if (!prevMaster) return rs.getMaster()
    var rec = prevMaster.getDB('admin').runCommand({ismaster: 1})
    if (rec && rec.ok && rec['ismaster']) return prevMaster
    return rs.getMaster()
}

function queryAndUpdateData(rs) {return function(z) {return function(i) {
  try {
    sleep((z + 5) * 1000)
    print('update ' + z + ' round ' + i)
    var db
    try {
        var master = rsMaster(rs)
        if (!master) {print('no master, retry soon'); return}
        db = master.getDB('test')
    } catch (e) {
        print ('get master failed (down primary), retry soon: ' + e)
        return
    }
    var n
    try {
        db['col'].update({}, {$push: {'z': z}}, false, true)
        n = db['col'].count({'z': z})
    } catch (e) {
        print('query failed (down primary), retry soon: ' + e)
        return
    }
    checkEqual (n, N)
    sleep(1000)
    try {
        db['col'].update({}, {$pull: {'z': z}}, false, true)
        n = db['col'].count({'z': z})
    } catch (e) {
        print('query failed (down primary), retry soon: ' + e)
        return
    }
    checkEqual (n, 0)
  } catch (e) {throw ('(Q&U' + z + '-' + i + ') ' + e)}
}}}

function killer(rs) {return function(i) {
  try {
    sleep(random(60) * 1000)
    var r = random(rs.ports.length)
    print('Killing ' + r)
    stopMongod(rs.getPort(r), 9)  // hard kill
    sleep(random(30) * 1000)
    print('Restarting ' + r)
    rs.restart(r, {dur: null})
  } catch (e) {throw ('(Killer-' + i + ') ' + e)}
}}

function go(numRounds) {
    var rs = deploy()
    loadInitialData(rs)
    var jobs = map(queryAndUpdateData(rs), [1,2,3,4,5])
    parallel (numRounds, jobs, [killer(rs)])
    sleep (2000)
    rs.stopSet()
    print("rsDurKillRestart1.js SUCCESS")
}
