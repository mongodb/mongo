//
// Tests sharding-related batch write protocol functionality
// NOTE: Basic write functionality is tested via the passthrough tests, this file should contain
// *only* mongos-specific tests.
//

var options = { separateConfig : true, sync : true };
var st = new ShardingTest({shards: 2, mongos: 1, other: options});
st.stopBalancer();

var mongos = st.s0;
var admin = mongos.getDB( "admin" );
var config = mongos.getDB( "config" );

jsTest.log("Starting insert tests...");

var request;
var result;

function resultOK( result ) {
    return result.ok &&
           !( 'code' in result ) &&
           !( 'errmsg' in result ) &&
           !( 'errInfo' in result ) &&
           !( 'errDetails' in result );
};

function resultNOK( result ) {
    return !result.ok &&
           typeof( result.code ) == 'number' &&
           typeof( result.errmsg ) == 'string';
}


var request;
var result;

// NOTE: ALL TESTS BELOW SHOULD BE SELF-CONTAINED, FOR EASIER DEBUGGING

//
//
// Tests against config server
var configColl = config.getCollection( "batch_write_protocol_sharded" );

//
// Basic config server insert
configColl.remove({});
printjson( request = {insert : configColl.getName(),
                      documents: [{a:1}]} );
printjson( result = configColl.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(1, st.config0.getCollection(configColl + "").count());
assert.eq(1, st.config1.getCollection(configColl + "").count());
assert.eq(1, st.config2.getCollection(configColl + "").count());

//
// Basic config server update
configColl.remove({});
configColl.insert({a:1});
printjson( request = {update : configColl.getName(),
                      updates: [{q: {a:1}, u: {$set: {b:2}}}]} );
printjson( result = configColl.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(1, st.config0.getCollection(configColl + "").count({b:2}));
assert.eq(1, st.config1.getCollection(configColl + "").count({b:2}));
assert.eq(1, st.config2.getCollection(configColl + "").count({b:2}));

//
// Basic config server delete
configColl.remove({});
configColl.insert({a:1});
printjson( request = {'delete' : configColl.getName(),
                      deletes: [{q: {a:1}, limit: 0}]} );
printjson( result = configColl.runCommand(request) );
assert(resultOK(result));
assert.eq(1, result.n);
assert.eq(0, st.config0.getCollection(configColl + "").count());
assert.eq(0, st.config1.getCollection(configColl + "").count());
assert.eq(0, st.config2.getCollection(configColl + "").count());


//
// TODO: More tests to come
//

jsTest.log("DONE!");
st.stop();

