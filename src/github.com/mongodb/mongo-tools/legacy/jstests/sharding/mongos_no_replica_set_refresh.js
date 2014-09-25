// Tests whether new sharding is detected on insert by mongos

var st = new ShardingTest( name = "test", shards = 1, verbose = 2, mongos = 2, other = { rs : true } )

var mongos = st.s
var config = mongos.getDB("config")

config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true )

printjson( mongos.getCollection("foo.bar").findOne() )

var rsObj = st._rs[0].test
var primary = rsObj.getPrimary()
var secondaries = rsObj.getSecondaries()

printjson( primary.getDB("local").system.replset.findOne() )

jsTestLog( "Reconfiguring replica set..." )

var reconfig = { _id : rsObj.name, version : 2, members : [ { _id : 0, host : primary.host }, { _id : 1, host : secondaries[0].host } ] }
try{
    // Results in a stepdown, which results in an exception
    printjson( primary.getDB("admin").runCommand({ replSetReconfig : reconfig }) )
}
catch( e ){
    printjson( e )
}

var numRSHosts = function(){
    var result = primary.getDB("admin").runCommand({ ismaster : 1 })
    printjson( result )
    return result.hosts.length
}

assert.soon( function(){ return numRSHosts() < 3 } )

var numMongosHosts = function(){
    var result = mongos.getDB("admin").runCommand("connPoolStats")["replicaSets"][ rsObj.name ]
    printjson( result )
    return result.hosts.length
}

assert.soon( function(){ return numMongosHosts() < 3 } )

jsTestLog( "Mongos successfully detected change..." )

var configServerURL = function(){
    var result = config.shards.find().toArray()[0]
    printjson( result )
    return result.host
}

assert.soon( function(){ return configServerURL().indexOf( secondaries[1].host ) < 0 } )

jsTestLog( "Now test adding new replica set servers..." )

config.shards.update({ _id : rsObj.name }, { $set : { host : rsObj.name + "/" + primary.host } })
printjson( config.shards.find().toArray() )

var reconfig = { _id : rsObj.name, version : 3, members : [ { _id : 0, host : primary.host }, { _id : 1, host : secondaries[0].host }, { _id : 2, host : secondaries[1].host } ] }

try {
    // Results in a stepdown, which results in an exception
    printjson( primary.getDB("admin").runCommand({ replSetReconfig : reconfig }) )
}
catch( e ){
    printjson( e )
}

assert.soon( function(){ return numRSHosts() > 2 } )

assert.soon( function(){ return numMongosHosts() > 2 } )

assert.soon( function(){ return configServerURL().indexOf( secondaries[1].host ) >= 0 } )

jsTestLog( "Done..." )

st.stop()