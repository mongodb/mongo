// Change a getLastErrorMode from 2 to 3 servers

var host = getHostName();
var replTest = new ReplSetTest( {name: "rstag", nodes: 3, startPort: 31000} );
var nodes = replTest.startSet();
var ports = replTest.ports;
var conf = {_id : "rstag", version: 1, members : [
    {_id : 0, host : host+":"+ports[0], tags : ["backup"]},
    {_id : 1, host : host+":"+ports[1], tags : ["backup"]},
    {_id : 2, host : host+":"+ports[2], tags : ["backup"]} ],
            settings : {getLastErrorModes : {
                backedUp : {backup : 2} }} };
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

replTest.stopSet();
