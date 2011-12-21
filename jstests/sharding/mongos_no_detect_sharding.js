// Tests whether new sharding is detected on insert by mongos

var st = new ShardingTest( name = "test", shards = 1, verbose = 2, mongos = 2, other = { separateConfig : true } )

var mongos = st.s
var config = mongos.getDB("config")

config.settings.update({ _id : "balancer" }, { $set : { stopped : true } }, true )


print( "Creating unsharded connection..." )


var mongos2 = st._mongos[1]

var coll = mongos2.getCollection( "test.foo" )
coll.insert({ i : 0 })

print( "Sharding collection..." )

var admin = mongos.getDB("admin")

assert.eq( coll.getShardVersion().ok, 0 )

admin.runCommand({ enableSharding : "test" })
admin.runCommand({ shardCollection : "test.foo", key : { _id : 1 } })

print( "Seeing if data gets inserted unsharded..." )
print( "No splits occur here!" )

// Insert a bunch of data which should trigger a split
for( var i = 0; i < 100; i++ ){
    coll.insert({ i : i + 1 })
}
coll.getDB().getLastError()

config.printShardingStatus( true )

assert.eq( coll.getShardVersion().ok, 1 )
assert.eq( 101, coll.find().itcount() )

st.stop()