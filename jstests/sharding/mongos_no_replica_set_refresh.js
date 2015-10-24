// Tests whether new sharding is detected on insert by mongos
load("jstests/replsets/rslib.js");
(function () {
var st = new ShardingTest(
    name = "test",
    shards = 1,
    verbose = 2,
    mongos = 2,
    other = {
        rs0: {
            nodes: [
                {rsConfig: {priority: 10}},
                {},
                {},
            ],
        },
    }
);

var rsObj = st._rs[0].test;
assert.commandWorked(
    rsObj.nodes[0].adminCommand({
        replSetTest: 1,
        waitForMemberState: rsObj.PRIMARY,
        timeoutMillis: 60 * 1000,
    }),
    'node 0 ' + rsObj.nodes[0].host + ' failed to become primary'
);

var mongos = st.s;
var config = mongos.getDB("config");

config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true );

printjson( mongos.getCollection("foo.bar").findOne() );

var primary = rsObj.getPrimary();

jsTestLog( "Reconfiguring replica set..." );

var rsConfig = rsObj.getConfigFromPrimary();

// Now remove the last node in the config.
var removedNode = rsConfig.members.pop();
rsConfig.version++;
reconfig(rsObj, rsConfig);

var numRSHosts = function(){
    jsTestLog('Checking number of active nodes in ' + rsObj.name);
    var result = assert.commandWorked(primary.adminCommand({ismaster : 1}));
    jsTestLog('Active nodes in ' + rsObj.name + ': ' + tojson(result));
    return result.hosts.length;
};

primary = rsObj.getPrimary();
assert.soon( function(){ return numRSHosts() < 3; } );

var numMongosHosts = function(){
    jsTestLog('Checking number of nodes in ' + rsObj.name + ' connected to mongos...');
    var commandResult = assert.commandWorked(mongos.adminCommand("connPoolStats"));
    var result = commandResult.replicaSets[rsObj.name];
    jsTestLog('Nodes in ' + rsObj.name + ' connected to mongos: ' + tojson(result));
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

rsConfig.members.push(removedNode);
rsConfig.version++;
reconfig(rsObj, rsConfig);

assert.soon( function(){ return numRSHosts() > 2; } );

assert.soon( function(){ return numMongosHosts() > 2; } );

assert.soon( function(){ return configServerURL().indexOf( removedNode.host ) >= 0; } );

jsTestLog( "Done..." );

st.stop();
}());
