1/**
 * This tests whether slaveOk reads are properly routed through mongos in
 * an authenticated environment. This test also includes restarting the
 * entire set, then querying afterwards.
 */

/**
 * Checks if a query to the given collection will be routed to the secondary.
 *
 * @param {DBCollection} coll
 * @param {Object} query
 *
 * @return {boolean} true if query was routed to a secondary node.
 */
function doesRouteToSec( coll, query ) {
    var explain = coll.find( query ).explain();
    var conn = new Mongo( explain.server );
    var cmdRes = conn.getDB( 'admin' ).runCommand({ isMaster: 1 });

    jsTest.log('isMaster: ' + tojson(cmdRes));

    return cmdRes.secondary;
}

var rsOpts = { oplogSize: 10 };
var st = new ShardingTest({ keyFile: 'jstests/libs/key1', shards: 1,
    rs: rsOpts, other: { nopreallocj: 1 }});

var mongos = st.s;
var replTest = st.rs0;
var testDB = mongos.getDB( 'AAAAA' );
var coll = testDB.user;
var nodeCount = replTest.nodes.length;

/* Add an admin user to the replica member to simulate connecting from
 * remote location. This is because mongod allows unautheticated
 * connections to access the server from localhost connections if there
 * is no admin user.
 */
var adminDB = mongos.getDB( 'admin' )
adminDB.addUser('user', 'password', jsTest.adminUserRoles);
adminDB.auth( 'user', 'password' );
var priAdminDB = replTest.getPrimary().getDB( 'admin' );
priAdminDB.addUser( 'user', 'password', jsTest.adminUserRoles, 3 );

coll.drop();
coll.setSlaveOk( true );

/* Secondaries should be up here, but they can still be in RECOVERY
 * state, which will make the ReplicaSetMonitor mark them as
 * ok = false and not eligible for slaveOk queries.
 */
ReplSetTest.awaitRSClientHosts( mongos, replTest.getSecondaries(),
   { ok : true, secondary : true });

for ( var x = 0; x < 20; x++ ) {
    coll.insert({ v: x, k: 10 });
}

coll.runCommand({ getLastError: 1, w: nodeCount });

/* Although mongos never caches query results, try to do a different query
 * everytime just to be sure.
 */
var vToFind = 0;

jsTest.log( 'First query to SEC' );
assert( doesRouteToSec( coll, { v: vToFind++ }));

var SIG_TERM = 15;
replTest.stopSet( SIG_TERM, true, { auth: { user: 'user', pwd: 'password' }});

for ( var n = 0; n < nodeCount; n++ ) {
    replTest.restart( n, rsOpts );
}

replTest.awaitSecondaryNodes();

coll.setSlaveOk( true );

/* replSetMonitor does not refresh the nodes information when getting secondaries.
 * A node that is previously labeled as secondary can now be a primary, so we
 * wait for the replSetMonitorWatcher thread to refresh the nodes information.
 */
ReplSetTest.awaitRSClientHosts( mongos, replTest.getSecondaries(),
    { ok : true, secondary : true });
//
// We also need to wait for the primary, it's possible that the mongos may think a node is a 
// secondary but it actually changed to a primary before we send our final query.
//
ReplSetTest.awaitRSClientHosts( mongos, replTest.getPrimary(),
    { ok : true, ismaster : true });

// Recheck if we can still query secondaries after refreshing connections.
jsTest.log( 'Final query to SEC' );
assert( doesRouteToSec( coll, { v: vToFind++ }));

// Cleanup auth so Windows will be able to shutdown gracefully
priAdminDB = replTest.getPrimary().getDB( 'admin' );
priAdminDB.auth( 'user', 'password' );
priAdminDB.removeUser( 'user' );

st.stop();

