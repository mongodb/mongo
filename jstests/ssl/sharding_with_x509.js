// Tests basic sharding with x509 cluster auth 
// The purpose is to verify the connectivity between mongos and the shards

var x509_options = {sslOnNormalPorts : "",
                    sslPEMKeyFile : "jstests/libs/server.pem",
                    sslCAFile: "jstests/libs/ca.pem",
                    sslClusterFile: "jstests/libs/cluster-cert.pem",
                    clusterAuthMode: "x509"};

var st = new ShardingTest({ name : "sharding_with_x509" ,
                            shards : 2,
                            mongos : 1,
                            keyFile : "jstests/libs/key1",
                            other: {
                                configOptions : x509_options,
                                mongosOptions : x509_options,
                                rsOptions : x509_options,
                                shardOptions : x509_options
                            }});

var mongos = new Mongo( "localhost:" + st.s0.port )
var coll = mongos.getCollection( "test.foo" )

st.shardColl( coll, { _id : 1 }, false )

// Create an index so we can find by num later
coll.ensureIndex({ insert : 1 })

print( "starting insertion phase" )

// Insert a bunch of data
var toInsert = 2000
for( var i = 0; i < toInsert; i++ ){
    coll.insert({ my : "test", data : "to", insert : i })
}

assert.eq( coll.getDB().getLastError(), null )

print( "starting updating phase" )

// Update a bunch of data
var toUpdate = toInsert
for( var i = 0; i < toUpdate; i++ ){
    var id = coll.findOne({ insert : i })._id
    coll.update({ insert : i, _id : id }, { $inc : { counter : 1 } })
}

assert.eq( coll.getDB().getLastError(), null )

print( "starting deletion" )

// Remove a bunch of data
var toDelete = toInsert / 2
for( var i = 0; i < toDelete; i++ ){
    coll.remove({ insert : i })
}

assert.eq( coll.getDB().getLastError(), null )

// Make sure the right amount of data is there
assert.eq( coll.find().count(), toInsert / 2 )

// Finish
st.stop()
