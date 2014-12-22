// Tests whether new sharding is detected on insert by mongos
load("jstests/replsets/rslib.js");
(function () {
var st = new ShardingTest(name = "test", 
                          shards = 1, 
                          verbose = 2, 
                          mongos = 2, 
                          other = { rs : true });

var mongos = st.s;
var config = mongos.getDB("config");

config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true );

printjson( mongos.getCollection("foo.bar").findOne() );

var rsObj = st._rs[0].test;
var primary = rsObj.getPrimary();
var secondaries = rsObj.getSecondaries();

var rsConfig = primary.getDB("local").system.replset.findOne();

jsTestLog( "Reconfiguring replica set..." );

var removedNode = rsConfig.members.pop();
rsConfig.version++;

// Need to force in case the node being removed is the current primary
reconfig(rsObj, rsConfig, true);
primary = rsObj.getPrimary();

var numRSHosts = function(){
    var result = primary.getDB("admin").runCommand({ ismaster : 1 });
    printjson( result );
    return result.hosts.length;
};

primary = rsObj.getPrimary();
assert.soon( function(){ return numRSHosts() < 3; } );

var numMongosHosts = function(){
    var result = mongos.getDB("admin").runCommand("connPoolStats")["replicaSets"][ rsObj.name ];
    printjson( result );
    return result.hosts.length;
};

// Wait for ReplicaSetMonitor to refresh; it should discover that the set now has only 2 hosts.
assert.soon( function(){ return numMongosHosts() < 3; } );

jsTestLog( "Mongos successfully detected change..." );

var configServerURL = function(){
    var result = config.shards.find().toArray()[0];
    printjson( result );
    return result.host;
};

assert.soon( function(){ return configServerURL().indexOf( removedNode.host ) < 0; } );

jsTestLog( "Now test adding new replica set servers..." );

config.shards.update({ _id : rsObj.name }, { $set : { host : rsObj.name + "/" + primary.host } });
printjson( config.shards.find().toArray() );

rsConfig = primary.getDB("local").system.replset.findOne();
rsConfig.members.push(removedNode);
rsConfig.version++;
reconfig(rsObj, rsConfig);

assert.soon( function(){ return numRSHosts() > 2; } );

assert.soon( function(){ return numMongosHosts() > 2; } );

assert.soon( function(){ return configServerURL().indexOf( removedNode.host ) >= 0; } );

jsTestLog( "Done..." );

st.stop();
}());
