// Validates that, when it cannot reach a config server, bongos assumes that the
// localhost exception does not apply.  That is, if bongos cannot verify that there
// are user documents stored in the configuration information, it must assume that
// there are.

var dopts = {smallfiles: "", nopreallocj: ""};
var st = new ShardingTest({
    shards: 1,
    bongos: 1,
    config: 1,
    keyFile: 'jstests/libs/key1',
    useHostname: false,  // Needed when relying on the localhost exception
    other: {shardOptions: dopts, configOptions: dopts, bongosOptions: {verbose: 1}}
});
var bongos = st.s;
var config = st.config0;
var authzErrorCode = 13;

// set up user/pwd on admin db with clusterAdmin role (for serverStatus)
var conn = new Bongo(bongos.host);
var db = conn.getDB('admin');
db.createUser({user: 'user', pwd: 'pwd', roles: ['clusterAdmin']});
db.auth('user', 'pwd');

// open a new connection to bongos (unauthorized)
var conn = new Bongo(bongos.host);
db = conn.getDB('admin');

// first serverStatus should fail since user is not authorized
assert.commandFailedWithCode(db.adminCommand('serverStatus'), authzErrorCode);

// authorize and repeat command, works
db.auth('user', 'pwd');
assert.commandWorked(db.adminCommand('serverStatus'));

jsTest.log('repeat without config server');

// shut down only config server
BongoRunner.stopBongod(config.port, /*signal*/ 15);

// open a new connection to bongos (unauthorized)
var conn2 = new Bongo(bongos.host);
var db2 = conn2.getDB('admin');

// should fail since user is not authorized.
assert.commandFailedWithCode(db2.adminCommand('serverStatus'), authzErrorCode);
