// Tests slaveok failover for shards

var numShards = 1
// NOTE: New primary won't be elected with two replicas
var numReplicas = 2
var numMongos = 1

var verboseLevel = 0

var replicaSets = []
var configServers = []
var mongos = []

localhost = getHostName()

print( "\n\n\nSTARTING REPLICA SETS FOR SHARDS" )

// NOTE: Cannot use i? as the repltest seems to pollute the namespace
for ( var shardNum = 0; shardNum < numShards; shardNum++ ) {
	var setName = "testReplicaSet-" + shardNum;
	var rt = new ReplSetTest( { name : setName, nodes : numReplicas, startPort : 31100 + ( shardNum * 100 ) } )
	rt.startSet( {} ) //{ logpath : '/dev/null', logappend : null } )

	// Start Syncing sets
	rt.initiate()
	rt.getMaster().getDB( "admin" )
	rt.awaitReplication()

	for ( var i = 0; i < rt.nodes.length; i++ ) {
		print("Is Slave OK? " + rt.nodes[i].slaveOk)
	}

	print( "\n\n\nADDING REPLICA SET " + shardNum )
	replicaSets.push( rt )
}

print( "\n\n\nSTARTING CONFIG SERVERS" )

// Start config servers
for ( var configNum = 0; configNum < 3; configNum++ ) {
	var configName = "testConfigServer-" + configNum;
	var conn = startMongodTest( 30001 + configNum, configName, false, {}) //, {
		//logpath : '/tmp/shard_shutdown.js/configServer.' + configNum + '.log', logappend : "" } );
	configServers.push( conn );
}

// Get link to config servers
var configConnStr = localhost + ":30001," + localhost + ":30002," + localhost + ":30003"
var configConns = new Mongo( configConnStr );

print( "\n\n\nSTARTING MONGOS INSTANCES" )

// Create mongos instances
for ( var mongoNum = 0; mongoNum < numMongos; mongoNum++ ) {
	var conn = startMongos( {
		port : 30000 - mongoNum, v : verboseLevel, configdb : configConnStr })
		//logpath : '/tmp/shard_shutdown.js/mongos.' + mongoNum + ".log", logappend : "" } );

	conn.setSlaveOk()
	mongos.push( conn )

	// Add shards to the mongos instance
	for ( var shardNum = 0; shardNum < numShards; shardNum++ ) {
		conn.getDB( "admin" ).runCommand( { addshard : replicaSets[shardNum].getURL() } )
	}
}

// Kill balancer
// mongos[0].getDB( "admin" ).settings.update( { _id : "balancer" }, { $set : { stopped : true } }, true )

// Add a bunch of stuff to the shards
var dbName = "foobar"
var collName = "baz"
var ns = dbName + "." + collName
var db = mongos[0].getDB( dbName )
var coll = db.getCollection( collName )

for ( var i = 0; i < 300; i++ )
	coll.insert( { value : i, name : "xyz-" + i } )

	
// Stop the primary of the replica set the data is on
var primaryName = mongos[0].getDB( "config" ).databases.findOne( { _id : dbName } ).primary

var getReplicaSet = function(name) {
	for ( var i = 0; i < replicaSets.length; i++ ) {
		if (replicaSets[i].name == name)
			return replicaSets[i]
	}
}

var primaryRS = getReplicaSet( primaryName )

var getMasterIndex = function(rs) {
	var masterConn = rs.getMaster()
	for ( var i = 0; i < rs.nodes.length; i++ ) {
		if (rs.nodes[i] == masterConn)
			return i
	}
}

var masterIndex = getMasterIndex( primaryRS )
primaryRS.awaitReplication()

primaryRS.stop( masterIndex )

sleep(1000)

// By default, if the output is undefined we do an error check, which fails, and should not bork the connection
// for further slaveok requests.

var firstDoc = coll.findOne(); 1

try{
	db.getLastError()
}
catch(e){}

printjson( firstDoc = coll.findOne() ); 1

sleep(50)

printjson( firstDoc = coll.findOne() ); 1



