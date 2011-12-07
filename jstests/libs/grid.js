// Grid infrastructure: Servers, ReplicaSets, ConfigSets, Shards, Routers (mongos). Convenient objects and functions on top of those in shell/servers.js -Tony

load('jstests/libs/fun.js')
load('jstests/libs/network.js')

// New servers and routers take and increment port number from this.
// A comment containing FreshPorts monad implies reading and incrementing this, IO may also read/increment this.
var nextPort = 31000

/*** Server is the spec of a mongod, ie. all its command line options.
 To start a server call 'begin' ***/
// new Server :: String -> FreshPorts Server
function Server (name) {
    this.addr = '127.0.0.1';
    this.dirname = name + nextPort;
    this.args = { port : nextPort++,
                  noprealloc : '',
                  smallfiles : '',
                  rest : '',
                  oplogSize : 8 }
}

// Server -> String <addr:port>
Server.prototype.host = function() {
    return this.addr + ':' + this.args.port
}

// Start a new server with this spec and return connection to it
// Server -> IO Connection
Server.prototype.begin = function() {
    return startMongodTest(this.args.port, this.dirname, false, this.args);
}

// Stop server and remove db directory
// Server -> IO ()
Server.prototype.end = function() {
    print('Stopping mongod on port ' + this.args.port)
    stopMongod (this.args.port)
    resetDbpath ('/data/db/' + this.dirname)
}

// Cut server from network so it is unreachable (but still alive)
// Requires sudo access and ipfw program (Mac OS X and BSD Unix). TODO: use iptables on Linux.
function cutServer (conn) {
    var addrport = parseHost (conn.host)
    cutNetwork (addrport.port)
}

// Ensure server is connected to network (undo cutServer)
// Requires sudo access and ipfw program (Mac OS X and BSD Unix). TODO: use iptables on Linux.
function uncutServer (conn) {
    var iport = parseHost (conn.host)
    restoreNetwork (iport.port)
}

// Kill server process at other end of this connection
function killServer (conn, _signal) {
    var signal = _signal || 15
    var iport = parseHost (conn.host)
    stopMongod (iport.port, signal)
}

/*** ReplicaSet is the spec of a replica set, ie. options given to ReplicaSetTest.
 To start a replica set call 'begin' ***/
// new ReplicaSet :: String -> Int -> FreshPorts ReplicaSet
function ReplicaSet (name, numServers) {
    this.name = name
    this.host = '127.0.0.1'
    this.nodes = numServers
    this.startPort = nextPort
    this.oplogSize = 40
    nextPort += numServers
}

// Start a replica set with this spec and return ReplSetTest, which hold connections to the servers including the master server. Call ReplicaSetTest.stopSet() to end all servers
// ReplicaSet -> IO ReplicaSetTest
ReplicaSet.prototype.begin = function() {
    var rs = new ReplSetTest(this)
    rs.startSet()
    rs.initiate()
    rs.awaitReplication()
    return rs
}

// Create a new server and add it to replica set
// ReplicaSetTest -> IO Connection
ReplSetTest.prototype.addServer = function() {
    var conn = this.add()
    nextPort++
    this.reInitiate()
    this.awaitReplication()
    assert.soon(function() {
        var doc = conn.getDB('admin').isMaster()
        return doc['ismaster'] || doc['secondary']
    })
    return conn
}

/*** ConfigSet is a set of specs (Servers) for sharding config servers.
 Supply either the servers or the number of servers desired.
 To start the config servers call 'begin' ***/
// new ConfigSet :: [Server] or Int -> FreshPorts ConfigSet
function ConfigSet (configSvrsOrNumSvrs) {
    if (typeof configSvrsOrNumSvrs == 'number') {
        this.configSvrs = []
        for (var i = 0; i < configSvrsOrNumSvrs; i++)
            this.configSvrs.push (new Server ('config'))
    } else
        this.configSvrs = configSvrs
}

// Start config servers, return list of connections to them
// ConfigSet -> IO [Connection]
ConfigSet.prototype.begin = function() {
    return map (function(s) {return s.begin()}, this.configSvrs)
}

// Stop config servers
// ConfigSet -> IO ()
ConfigSet.prototype.end = function() {
    return map (function(s) {return s.end()}, this.configSvrs)
}

/*** Router is the spec for a mongos, ie, its command line options.
 To start a router (mongos) call 'begin' ***/
// new Router :: ConfigSet -> FreshPorts Router
function Router (configSet) {
    this.args = { port : nextPort++,
                  v : 0,
                  configdb : map (function(s) {return s.host()}, configSet.configSvrs) .join(','),
                  chunkSize : 1}
}

// Start router (mongos) with this spec and return connection to it.
// Router -> IO Connection
Router.prototype.begin = function() {
    return startMongos (this.args);
}

// Stop router
// Router -> IO ()
Router.prototype.end = function() {
    return stopMongoProgram (this.args.port)
}

// Add shard to config via router (mongos) connection. Shard is either a replSet name (replSet.getURL()) or single server (server.host)
// Connection -> String -> IO ()
function addShard (routerConn, repSetOrHostName) {
    var ack = routerConn.getDB('admin').runCommand ({addshard: repSetOrHostName})
    assert (ack['ok'], tojson(ack))
}

// Connection -> String -> IO ()
function enableSharding (routerConn, dbName) {
    var ack = routerConn.getDB('admin').runCommand ({enablesharding: dbName})
    assert (ack['ok'], tojson(ack))
}

// Connection -> String -> String -> String -> IO ()
function shardCollection (routerConn, dbName, collName, shardKey) {
    var ack = routerConn.getDB('admin').runCommand ({shardcollection: dbName + '.' + collName, key: shardKey})
    assert (ack['ok'], tojson(ack))
}

// Move db from its current primary shard to given shard. Shard is either a replSet name (replSet.getURL()) or single server (server.host)
// Connection -> String -> String -> IO ()
function moveDB (routerConn, dbname, repSetOrHostName) {
    var ack = routerConn.getDB('admin').runCommand ({moveprimary: dbname, to: repSetOrHostName})
    printjson(ack)
    assert (ack['ok'], tojson(ack))
}
