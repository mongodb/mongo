// Tests the dropping and re-adding of a collection

var st = new ShardingTest( name = "multidrop", shards = 1, verbose = 0, mongos = 2 )

var mA = st.s0
var mB = st.s1

var coll = mA.getCollection( name + ".coll" )
var collB = mB.getCollection( coll + "" )

jsTestLog( "Shard and split collection..." )

var admin = mA.getDB( "admin" )
admin.runCommand({ enableSharding : coll.getDB() + "" })
admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } })

for( var i = -100; i < 100; i++ ){
    admin.runCommand({ split : coll + "", middle : { _id : i } })
}

jsTestLog( "Create versioned connection for each mongos..." )

coll.find().itcount()
collB.find().itcount()

jsTestLog( "Dropping sharded collection..." )
coll.drop()

jsTestLog( "Recreating collection..." )

admin.runCommand({ shardCollection : coll + "", key : { _id : 1 } })
for( var i = -10; i < 10; i++ ){
    admin.runCommand({ split : coll + "", middle : { _id : i } })
}

jsTestLog( "Retrying connections..." )

coll.find().itcount()
collB.find().itcount() 

jsTestLog( "Done." )

st.stop()


