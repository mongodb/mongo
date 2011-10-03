// Change a getLastErrorMode from 2 to 3 servers

var host = getHostName();
var replTest = new ReplSetTest( {name: "rstag", nodes: 4, startPort: 31000} );
var nodes = replTest.startSet();
var ports = replTest.ports;
var conf = {_id : "rstag", version: 1, members : [
    {_id : 0, host : host+":"+ports[0], tags : {"backup" : "A"}},
    {_id : 1, host : host+":"+ports[1], tags : {"backup" : "B"}},
    {_id : 2, host : host+":"+ports[2], tags : {"backup" : "C"}},
    {_id : 3, host : host+":"+ports[3], tags : {"backup" : "D"}, arbiterOnly : true}],
            settings : {getLastErrorModes : {
                backedUp : {backup : 2} }} };

print("arbiters can't have tags");
var result = nodes[0].getDB("admin").runCommand({replSetInitiate : conf});
printjson(result);
assert.eq(result.ok, 0);

conf.members.pop();
replTest.stop(3);
replTest.remove(3);
replTest.initiate( conf );

replTest.awaitReplication();

master = replTest.getMaster();
var db = master.getDB("test");
db.foo.insert( {x:1} );
var result = db.runCommand( {getLastError:1, w:"backedUp", wtimeout:20000} );
assert.eq (result.err, null);

conf.version = 2;
conf.settings.getLastErrorModes.backedUp.backup = 3;
master.getDB("admin").runCommand( {replSetReconfig: conf} );
replTest.awaitReplication();

master = replTest.getMaster();
var db = master.getDB("test");
db.foo.insert( {x:2} );
var result = db.runCommand( {getLastError:1, w:"backedUp", wtimeout:20000} );
assert.eq (result.err, null);

conf.version = 3;
conf.members[0].priorty = 3;
conf.members[2].priorty = 0;
master.getDB("admin").runCommand( {replSetReconfig: conf} );

master = replTest.getMaster();
var db = master.getDB("test");
db.foo.insert( {x:3} );
var result = db.runCommand( {getLastError:1, w:"backedUp", wtimeout:20000} );
assert.eq (result.err, null);

replTest.stopSet();
