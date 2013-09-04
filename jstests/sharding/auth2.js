var st = new ShardingTest({ keyFile : 'jstests/libs/key1', shards : 2, chunksize : 1, config : 3, verbose : 2,
                            other : { nopreallocj : 1, verbose : 2, useHostname : true,
                                      configOptions : { verbose : 2 }}});

st.printShardingStatus();

var mongos = st.s;
var adminDB = mongos.getDB('admin');
var db = mongos.getDB('test')

adminDB.addUser('admin', 'password', jsTest.adminUserRoles);

jsTestLog( "Add user was successful" );


// Test for SERVER-6549, make sure that repeatedly logging in always passes.
for ( var i = 0; i < 100; i++ ) {
    adminDB = new Mongo( mongos.host ).getDB('admin');
    assert( adminDB.auth('admin', 'password'), "Auth failed on attempt #: " + i );
}

st.stop();