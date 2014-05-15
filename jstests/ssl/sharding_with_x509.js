// Tests basic sharding with x509 cluster auth 
// The purpose is to verify the connectivity between mongos and the shards

var x509_options = {sslMode : "requireSSL",
                    sslPEMKeyFile : "jstests/libs/server.pem",
                    sslCAFile: "jstests/libs/ca.pem",
                    sslClusterFile: "jstests/libs/cluster-cert.pem",
                    clusterAuthMode: "x509"};

var st = new ShardingTest({ name : "sharding_with_x509" ,
                            shards : 2,
                            mongos : 1,
                            other: {
                                configOptions : x509_options,
                                mongosOptions : x509_options,
                                rsOptions : x509_options,
                                shardOptions : x509_options
                            }});

st.s.getDB('admin').createUser({user: 'admin', pwd: 'pwd', roles: ['root']});
st.s.getDB('admin').auth('admin', 'pwd');
var coll = st.s.getCollection( "test.foo" )

st.shardColl( coll, { _id : 1 }, false )

// Create an index so we can find by num later
coll.ensureIndex({ insert : 1 })

print( "starting insertion phase" )

// Insert a bunch of data
var toInsert = 2000;
var bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toInsert; i++ ){
    bulk.insert({ my: "test", data: "to", insert: i });
}
assert.writeOK(bulk.execute());

print( "starting updating phase" )

// Update a bunch of data
var toUpdate = toInsert;
bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toUpdate; i++ ){
    var id = coll.findOne({ insert : i })._id;
    bulk.find({ insert : i, _id : id }).update({ $inc : { counter : 1 } });
}
assert.writeOK(bulk.execute());

print( "starting deletion" )

// Remove a bunch of data
var toDelete = toInsert / 2;
bulk = coll.initializeUnorderedBulkOp();
for( var i = 0; i < toDelete; i++ ){
    bulk.find({ insert : i }).remove();
}
assert.writeOK(bulk.execute());

// Make sure the right amount of data is there
assert.eq( coll.find().count(), toInsert / 2 )

// Finish
st.stop()
