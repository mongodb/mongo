// Tests whether new sharding is detected on insert by mongos
load("jstests/replsets/rslib.js");

(function () {
'use strict';

var st = new ShardingTest({ name: 'mongos_no_replica_set_refresh',
                            shards: 1,
                            mongos: 1,
                            other: {
                                rs0: {
                                    nodes: [
                                        {},
                                        {rsConfig: {priority: 0}},
                                        {rsConfig: {priority: 0}},
                                    ],
                                }
                            } });

var rsObj = st.rs0;
assert.commandWorked(
    rsObj.nodes[0].adminCommand({
        replSetTest: 1,
        waitForMemberState: ReplSetTest.State.PRIMARY,
        timeoutMillis: 60 * 1000,
    }),
    'node 0 ' + rsObj.nodes[0].host + ' failed to become primary'
);

var mongos = st.s;
var config = mongos.getDB("config");

printjson( mongos.getCollection("foo.bar").findOne() );

jsTestLog( "Reconfiguring replica set..." );

var rsConfig = rsObj.getRSConfig(0);

// Now remove the last node in the config.
var removedNode = rsConfig.members.pop();
rsConfig.version++;
reconfig(rsObj, rsConfig);

// Wait for the election round to complete
rsObj.getPrimary();

var numRSHosts = function(){
    var result = assert.commandWorked(rsObj.nodes[0].adminCommand({ismaster : 1}));
    jsTestLog('Nodes in ' + rsObj.name + ': ' + tojson(result));
    return result.hosts.length + result.passives.length;
};

assert.soon( function(){ return numRSHosts() < 3; } );

var numMongosHosts = function(){
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

config.shards.update({ _id : rsObj.name }, { $set : { host : rsObj.name + "/" + rsObj.nodes[0].host } });
printjson( config.shards.find().toArray() );

rsConfig.members.push(removedNode);
rsConfig.version++;
reconfig(rsObj, rsConfig);

assert.soon( function(){ return numRSHosts() > 2; } );

assert.soon( function(){ return numMongosHosts() > 2; } );

assert.soon( function(){ return configServerURL().indexOf( removedNode.host ) >= 0; } );

st.stop();

}());
