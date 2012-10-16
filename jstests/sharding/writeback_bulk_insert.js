//
// Tests whether a writeback error during bulk insert hangs GLE
//

jsTest.log("Starting sharded cluster...")

var st = new ShardingTest({shards : 1,
                           mongos : 3,
                           verbose : 2,
                           other : {separateConfig : true,
                                    mongosOptions : {noAutoSplit : ""}}})

st.stopBalancer()

var mongosA = st.s0
var mongosB = st.s1
var mongosC = st.s2

jsTest.log("Adding new collection...")

var collA = mongosA.getCollection(jsTestName() + ".coll")
collA.insert({hello : "world"})
assert.eq(null, collA.getDB().getLastError())

var collB = mongosB.getCollection("" + collA)
collB.insert({hello : "world"})
assert.eq(null, collB.getDB().getLastError())

var collC = mongosC.getCollection("" + collA)
collC.insert({hello : "world"})
assert.eq(null, collC.getDB().getLastError())

jsTest.log("Enabling sharding...")

printjson(mongosA.getDB("admin").runCommand({enableSharding : collA.getDB()
                                                              + ""}))
printjson(mongosA.getDB("admin").runCommand({shardCollection : collA + "",
                                             key : {_id : 1}}))

// MongoD doesn't know about the config shard version *until* MongoS tells it
collA.findOne()

jsTest.log("Preparing bulk insert...")

var data1MB = "x"
while (data1MB.length < 1024 * 1024)
    data1MB += data1MB;

var data7MB = ""
// Data now at 7MB
for ( var i = 0; i < 7; i++)
    data7MB += data1MB;

print("7MB object size is : " + Object.bsonsize({_id : 0,
                                                 d : data7MB}))

var dataCloseTo8MB = data7MB;
// WARNING - MAGIC NUMBERS HERE
// The idea is to exceed the 16MB limit by just enough so that the message gets
// passed in the
// shell, but adding additional writeback information could fail.
for ( var i = 0; i < 1031 * 1024 + 862; i++) {
    dataCloseTo8MB += "x"
}

print("Object size is: " + Object.bsonsize([{_id : 0,
                                             d : dataCloseTo8MB},
                                            {_id : 1,
                                             d : dataCloseTo8MB}]))

jsTest.log("Trigger wbl for mongosB...")

collB.insert([{_id : 0,
               d : dataCloseTo8MB},
              {_id : 1,
               d : dataCloseTo8MB}])

// Will hang if overflow is not detected correctly
jsTest.log("Waiting for GLE...")

// No error thrown with new batching logic.
assert.eq(null, collB.getDB().getLastError())

print("GLE correctly returned, error not thrown.")

assert.eq(5, collA.find().itcount())
assert.eq(5, collB.find().itcount())

var data8MB = "";
for ( var i = 0; i < 8; i++) {
    data8MB += data1MB;
}

print("Object size is: " + Object.bsonsize([{_id : 0,
                                             d : data8MB},
                                            {_id : 1,
                                             d : data8MB}]))

jsTest.log("Trigger wbl for mongosC...")

collC.insert([{_id : 2,
               d : data8MB},
              {_id : 3,
               d : data8MB}])

// Should succeed since our insert size is 16MB (plus very small overhead)
jsTest.log("Waiting for GLE...")

assert.eq(null, collC.getDB().getLastError())

print("GLE Successful...")

assert.eq(7, collA.find().itcount())
assert.eq(7, collB.find().itcount())
assert.eq(7, collC.find().itcount())

st.stop()
