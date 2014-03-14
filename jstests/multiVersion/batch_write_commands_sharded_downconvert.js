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
    // Basic insert, default WC
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1}]});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(1, result.n);
    assert.eq(1, coll.count());

    //
    // Basic insert, error on WC application with invalid wmode
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1}],
                          writeConcern: {w:'invalid'}});
    printjson( result = coll.runCommand(request) );
    assert(result.ok);
    assert.eq(1, result.n);
    assert(result.writeConcernError != null);
    assert.eq('string', typeof(result.writeConcernError.errmsg));
    assert.eq(1, coll.count());

    //
    // Basic ordered insert, insert error so no WC error despite invalid wmode
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                          documents: [{_id: 1, a:1, $invalid: true}],
                          writeConcern: {w:'invalid'}});
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
    // Basic unordered insert, insert error and WC invalid mode error
    coll.remove({});
    printjson( request = {insert : coll.getName(),
                       documents: [{_id: -1, a:1}, {_id: 1, a:1, $invalid: true}],
                       writeConcern: {w:'invalid'},
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

    // GLE behavior change in 2.6
    if (coll === coll26) {

        //
        // Basic no journal insert, command error with nojournal in 2.6 and mixed
        coll.remove({});
        printjson( request = {insert : coll.getName(),
                              documents: [{_id: -1, a:1},{_id: 1, a:1}],
                              writeConcern: {j:true}});
        printjson( result = coll.runCommand(request) );
        // Reported as a batch failure in 2.6 - which is wrapped in a
        // write error via mongos like other batch failures
        assert(result.ok);
        assert.eq(0, result.n);
        assert.eq(result.writeErrors.length, 1);
        assert.eq(result.writeErrors[0].index, 0);
        assert.eq(0, coll.count());
    }
    else if (coll === collMixed) {
        
        //
        // Basic no journal insert, command error with nojournal in 2.6 and mixed
        coll.remove({});
        printjson( request = {insert : coll.getName(),
                              documents: [{_id: -1, a:1},{_id: 1, a:1}],
                              writeConcern: {j:true}});
        printjson( result = coll.runCommand(request) );
        // Reported as a batch failure in 2.6 - which is wrapped in a
        // write error via mongos like other batch failures
        // The 2.4 shard reports a write concern error, but with the batch 
        // failure against the 2.6 shard the client must assume the wc was
        // not applied and so this is suppressed. 
        assert(result.ok);
        assert.eq(1, result.n);
        assert.eq(result.writeErrors.length, 1); 
        assert.eq(result.writeErrors[0].index, 1); 
        assert.eq(1, coll.count());
    }
    else {

        //
        // Basic no journal insert, WC error with nojournal in 2.4
        coll.remove({});
        printjson( request = {insert : coll.getName(),
                              documents: [{_id: -1, a:1},{_id: 1, a:1}],
                              writeConcern: {j:true}});
        printjson( result = coll.runCommand(request) );
        assert(result.ok);
        assert.eq(2, result.n);
        assert(result.writeConcernError != null);
        assert.eq('string', typeof(result.writeConcernError.errmsg));
        assert.eq(2, coll.count());
    }

};

testWriteBatches(coll24);
testWriteBatches(coll26);
testWriteBatches(collMixed);

st.stop();

jsTest.log("DONE!");

