(function() {

load("jstests/libs/mongostat.js");

baseName = "tool_discover";

replSetPorts = allocatePorts(4);

port = allocatePorts(1,replSetPorts[3]+1);

m = startMongod("--port", port[0], "--dbpath", MongoRunner.dataPath + baseName + port[0], "--nohttpinterface", "--bind_ip", "127.0.0.1");

rs = new ReplSetTest({
    name: "rpls",
    nodes: 4,
    startPort: replSetPorts[0],
    useHostName: true
});

rs.startSet();

rs.initiate();

rs.awaitReplication();

rsFoo = rs.getMaster().getDB("foo");

rsFoo.bar.insert({
    a: 1
});

assert(discoverTest(replSetPorts, rs.liveNodes.master.host), "--discover against a replset master sees all members");

assert(discoverTest(replSetPorts, rs.liveNodes.slaves[0].host), "--discover against a replset slave sees all members");

assert(statOutputPortCheck([ rs.liveNodes.master.port, rs.liveNodes.slaves[0].port, rs.liveNodes.slaves[1].port ], [ "mongostat", "--host", "rpls/" + rs.liveNodes.master.host + "," + rs.liveNodes.slaves[0].host + "," + rs.liveNodes.slaves[1].host, "--rowcount", 1 ]), "replicata set specifiers are correctly used");

assert(discoverTest([ port[0] ], m.host), "--discover against a stand alone-sees just the stand-alone");

clearRawMongoProgramOutput();

x = runMongoProgram("mongostat", "--host", m.host, "--rowcount", 7, "--noheaders");

foundRows = rawMongoProgramOutput().split("\n").filter(function(r) {
    return r.match(rowRegex);
});

assert.eq(foundRows.length, 7, "--rowcount value is respected correctly");

startTime = new Date();

x = runMongoProgram("mongostat", "--host", m.host, "--rowcount", 3, "--noheaders", 3);

endTime = new Date();

duration = Math.floor((endTime.valueOf() - startTime.valueOf()) / 1000);

assert.gte(duration, 9, "sleep time affects the total time to produce a number or results");

clearRawMongoProgramOutput();
pid = startMongoProgramNoConnect("mongostat", "--host", rs.liveNodes.slaves[1].host, "--discover");

sleep(20000);

assert(statOutputPortCheck([ rs.liveNodes.slaves[1].port ]), "specified host is seen");

assert(statOutputPortCheck([ rs.liveNodes.slaves[0].port ]), "discovered host is seen");

rs.stop(rs.liveNodes.slaves[0]);

sleep(15000);

clearRawMongoProgramOutput();

sleep(2000);

assert(statOutputPortCheck([ rs.liveNodes.slaves[1].port ]), "after discovered host is stopped, specified host is still seen");

assert(!statOutputPortCheck([ rs.liveNodes.slaves[0].port ]), "after discovered host is stopped, it is not seen");

rs.start(rs.liveNodes.slaves[0]);

sleep(2000);

clearRawMongoProgramOutput();

sleep(2000);

assert(statOutputPortCheck([ rs.liveNodes.slaves[1].port ]), "after discovered is restarted, specified host is still seen");

assert(statOutputPortCheck([ rs.liveNodes.slaves[0].port ]), "after discovered is restarted, discovered host is seen again");

rs.stop(rs.liveNodes.slaves[1]);

assert.soon( function() {
            try {
                conn = new Mongo(rs.liveNodes.slaves[1].host);
                return false;
            } catch( e ) {
                return true;
            }
            return false;
}, "mongod still available after being stopped "+ rs.liveNodes.slaves[1].host);

sleep(1000);

clearRawMongoProgramOutput();

sleep(2000)

assert(!statOutputPortCheck([ rs.liveNodes.slaves[1].port ]), "after specified host is stopped, specified host is not seen");

assert(statOutputPortCheck([ rs.liveNodes.slaves[0].port ]), "after specified host is stopped, the discovered host is still seen");

//TODO test discover over a comma separated list of nodes from different replica sets.
}());
