/**
 * This test induces write concern errors in batched writes against (mixed) sharded clusters of
 * v2.4 and v2.6 replica set shards to ensure the batch write behavior is correct when
 * downconversion is used.
 */

// Options for a cluster with two one-node replica set shards, shard0 is v2.4, shard1 is v2.6
var options = { separateConfig : true,
                rs : true,
                configOptions : { binVersion : "2.4" },
                rsOptions : { nodes : 1,
                              nojournal : "" },
                // Options for each replica set shard
                rs0 : { binVersion : "2.4" },
                rs1 : { binVersion : "2.6" },
                mongosOptions : { binVersion : "2.6" } };

var st = new ShardingTest({ shards : 2, mongos : 1, other : options });
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB("admin");
var shards = mongos.getCollection("config.shards").find().toArray();
var coll24 = mongos.getCollection("test24.coll24");
var collMixed = mongos.getCollection("testMixed.collMixed");
var coll26 = mongos.getCollection("test26.coll26");

// Move the primary of the collections to the appropriate shards
coll24.drop();
collMixed.drop();
coll26.drop();
printjson(admin.runCommand({ movePrimary : coll24.getDB().toString(),
                             to : shards[0]._id }));
printjson(admin.runCommand({ movePrimary : collMixed.getDB().toString(),
                             to : shards[0]._id }));
printjson(admin.runCommand({ movePrimary : coll26.getDB().toString(),
                             to : shards[1]._id }));

// The mixed collection spans shards
assert.commandWorked(admin.runCommand({ enableSharding : collMixed.getDB().toString() }));
assert.commandWorked(admin.runCommand({ shardCollection : collMixed.toString(),
                                        key : { _id : 1 } }));
assert.commandWorked(admin.runCommand({ split : collMixed.toString(),
                                        middle : { _id : 0 } }));
assert.commandWorked(admin.runCommand({ moveChunk : collMixed.toString(),
                                        find : { _id : 0 },
                                        to : shards[1]._id }));

st.printShardingStatus();

var testWriteBatches = function (coll) {

    //
    //
    // Insert tests

    //
    // Basic no journal insert, default WC
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1}]});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(1, result.n);

    assert.eq(1, coll.count());

    //
    // Basic no journal insert, error on WC with j set
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1}],
                          writeConcern: {w:1, j:true}});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(1, result.n);
    assert(result.writeConcernError != null);
    assert.eq('string', typeof(result.writeConcernError.errmsg));

    assert.eq(1, coll.count());

    //
    // Basic no journal insert, insert error and no write
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1, $invalid: true}],
                          writeConcern: {w:1, j:true}});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(0, result.n);
    assert.eq(1, result.writeErrors.length);
    assert.eq(0, result.writeErrors[0].index);
    assert.eq('number', typeof result.writeErrors[0].code);
    assert.eq('string', typeof result.writeErrors[0].errmsg);
    assert(!('writeConcernError' in result));

    assert.eq(0, coll.count());

    //
    // Basic no journal insert, error on WC with j set and insert error
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                       documents: [{_id: -1, a:1}, {_id: 1, a:1, $invalid: true}],
                       writeConcern: {w:1, j:true},
                       ordered: false});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(1, result.n);
    assert.eq(1, result.writeErrors.length);

    assert.eq(1, result.writeErrors[0].index);
    assert.eq('number', typeof result.writeErrors[0].code);
    assert.eq('string', typeof result.writeErrors[0].errmsg);

    assert('writeConcernError' in result);
    assert.eq('string', typeof(result.writeConcernError.errmsg));

    assert.eq(1, coll.count());

    // TODO: More tests here

};

testWriteBatches(coll24);
testWriteBatches(coll26);
testWriteBatches(collMixed);

st.stop();

jsTest.log("DONE!");

