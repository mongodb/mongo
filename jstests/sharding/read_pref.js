/**
 * Integration test for read prefrence and tagging. The more comprehensive unit test
 * can be found in dbtests/replica_set_monitor_test.cpp.
 */

var PRI_TAG = { dc: 'ny' };
var SEC_TAGS = [
    { dc: 'sf', s: "1" },
    { dc: 'ma', s: "2" },
    { dc: 'eu', s: "3" },
    { dc: 'jp', s: "4" }
];
var NODES = SEC_TAGS.length + 1;

var st = new ShardingTest({ shards: { rs0: { nodes: NODES, oplogSize: 10,
                useHostName: true }}});
var replTest = st.rs0;
var primaryNode = replTest.getMaster();

var setupConf = function(){
    var replConf = primaryNode.getDB( 'local' ).system.replset.findOne();
    replConf.version = (replConf.version || 0) + 1;

    var secIdx = 0;
    for ( var x = 0; x < NODES; x++ ){
        var node = replConf.members[x];

        if ( node.host == primaryNode.name ){
            node.tags = PRI_TAG;
        }
        else {
            node.tags = SEC_TAGS[secIdx++];
        }
    }

    try {
        primaryNode.getDB( 'admin' ).runCommand({ replSetReconfig: replConf });
    } catch (x) {
        jsTest.log( 'Exception expected because reconfiguring would close all conn, got ' + x );
    }

    return replConf;
};

var replConf = setupConf();

var conn = st.s;

// Wait until the ReplicaSetMonitor refreshes its view and see the tags
ReplSetTest.awaitRSClientHosts( conn, primaryNode,
        { ok: true, tags: PRI_TAG }, replTest.name );
replTest.awaitReplication();

jsTest.log( 'New rs config: ' + tojson( primaryNode.getDB( 'local' ).system.replset.findOne() ));

jsTest.log( 'connpool: ' + tojson(conn.getDB('admin').runCommand({ connPoolStats: 1 })));

var coll = conn.getDB( 'test' ).user;

coll.insert({ x: 1 });
assert.eq( null, coll.getDB().getLastError( NODES ));

// Read pref should work without slaveOk
var explain = coll.find().readPref( "secondary" ).explain();
assert.neq( primaryNode.name, explain.server );

conn.setSlaveOk();

// It should also work with slaveOk
explain = coll.find().readPref( "secondary" ).explain();
assert.neq( primaryNode.name, explain.server );

// Check that $readPreference does not influence the actual query
assert.eq( 1, explain.n );

var checkTag = function( nodeToCheck, tag ){
    for ( var idx = 0; idx < NODES; idx++ ){
        var node = replConf.members[idx];

        if ( node.host == nodeToCheck ){
            jsTest.log( 'node[' + node.host + '], Tag: ' + tojson( node['tags'] ));
            jsTest.log( 'tagToCheck: ' + tojson( tag ));

            var nodeTag = node['tags'];

            for ( var key in tag ){
                assert.eq( tag[key], nodeTag[key] );
            }

            return;
        }
    }

    assert( false, 'node ' + nodeToCheck + ' not part of config!' );
};

explain = coll.find().readPref( "secondaryPreferred", [{ s: "2" }] ).explain();
checkTag( explain.server, { s: "2" });
assert.eq( 1, explain.n );

// Cannot use tags with primaryOnly
assert.throws( function() {
    coll.find().readPref( "primaryOnly", [] ).explain();
});

// Check that mongos will try the next tag if nothing matches the first
explain = coll.find().readPref( "secondary", [{ z: "3" }, { dc: "jp" }] ).explain();
checkTag( explain.server, { dc: "jp" });
assert.eq( 1, explain.n );

// Check that mongos will fallback to primary if none of tags given matches
explain = coll.find().readPref( "secondaryPreferred", [{ z: "3" }, { dc: "ph" }] ).explain();
// Call getPrimary again since the primary could have changed after the restart.
assert.eq(replTest.getPrimary().name, explain.server);
assert.eq( 1, explain.n );

// Kill all members except one
var stoppedNodes = [];
for ( var x = 0; x < NODES - 1; x++ ){
    replTest.stop( x );
    stoppedNodes.push( replTest.nodes[x] );
}

// Wait for ReplicaSetMonitor to realize nodes are down
ReplSetTest.awaitRSClientHosts( conn, stoppedNodes, { ok: false }, replTest.name );

// Wait for the last node to be in steady state -> secondary (not recovering)
var lastNode = replTest.nodes[NODES - 1];
ReplSetTest.awaitRSClientHosts( conn, lastNode,
    { ok: true, secondary: true }, replTest.name );

jsTest.log( 'connpool: ' + tojson(conn.getDB('admin').runCommand({ connPoolStats: 1 })));

// Test to make sure that connection is ok, in prep for priOnly test
explain = coll.find().readPref( "nearest" ).explain();
assert.eq( explain.server, replTest.nodes[NODES - 1].name );
assert.eq( 1, explain.n );

// Should assert if request with priOnly but no primary
assert.throws( function(){
   coll.find().readPref( "primary" ).explain();
});

st.stop();

