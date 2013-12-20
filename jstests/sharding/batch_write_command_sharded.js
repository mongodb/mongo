//
// Tests sharding-related batch write protocol functionality
// NOTE: Basic write functionality is tested via the passthrough tests, this file should contain
// *only* mongos-specific tests.
//

// Only reason for using localhost name is to make the test consistent with naming host so it
// will be easier to check for the host name inside error objects.
var options = { separateConfig : true, sync : true, configOptions: { useHostName: false }};
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
           !( 'writeErrors' in result );
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

MongoRunner.stopMongod(st.config1.port, 15);

// Config server insert with 2nd config down.
configColl.remove({});
printjson( request = {insert : configColl.getName(),
                      documents: [{a:1}]} );
printjson( result = configColl.runCommand(request) );
assert(resultNOK(result));
assert(result.errmsg != null);

//
// Config server update with 2nd config down.
configColl.remove({});
configColl.insert({a:1});
printjson( request = {update : configColl.getName(),
                      updates: [{q: {a:1}, u: {$set: {b:2}}}]} );
printjson( result = configColl.runCommand(request) );
assert(resultNOK(result));

//
// Config server delete with 2nd config down.
configColl.remove({});
configColl.insert({a:1});
printjson( request = {delete : configColl.getName(),
                      deletes: [{q: {a:1}, limit: 0}]} );
printjson( result = configColl.runCommand(request) );
assert(!resultOK(result));
assert(result.errmsg != null);

//
// Config server insert with 2nd config down while bypassing fsync check.
configColl.remove({});
printjson( request = { insert: configColl.getName(),
                       documents: [{ a: 1 }],
                       // { w: 0 } has special meaning for config servers
                       writeConcern: { w: 0 }} );
printjson( result = configColl.runCommand(request) );
assert(resultNOK(result));

//
// Config server update with 2nd config down while bypassing fsync check.
configColl.remove({});
configColl.insert({a:1});
printjson( request = { update: configColl.getName(),
                       updates: [{ q: { a: 1 }, u: { $set: { b:2 }}}],
                       // { w: 0 } has special meaning for config servers
                       writeConcern: { w: 0 }} );
printjson( result = configColl.runCommand(request) );
assert(resultNOK(result));

//
// Config server update with 2nd config down while bypassing fsync check.
configColl.remove({});
configColl.insert({a:1});
printjson( request = { delete: configColl.getName(),
                       deletes: [{ q: { a: 1 }, limit: 0 }],
                       // { w: 0 } has special meaning for config servers
                       writeConcern: { w: 0 }} );
printjson( result = configColl.runCommand(request) );
assert(resultNOK(result));
assert(result.errmsg != null);

//
// TODO: More tests to come
//

jsTest.log("DONE!");
st.stop();

