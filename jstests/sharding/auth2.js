(function() {
    'use strict';

    var st = new ShardingTest({
        shards: 2,
        other: {chunkSize: 1, useHostname: true, keyFile: 'jstests/libs/key1'},
    });

    var mongos = st.s;
    var adminDB = mongos.getDB('admin');
    var db = mongos.getDB('test');

    adminDB.createUser({user: 'admin', pwd: 'password', roles: jsTest.adminUserRoles});

    jsTestLog("Add user was successful");

    // Test for SERVER-6549, make sure that repeatedly logging in always passes.
    for (var i = 0; i < 100; i++) {
        adminDB = new Mongo(mongos.host).getDB('admin');
        assert(adminDB.auth('admin', 'password'), "Auth failed on attempt #: " + i);
    }

    st.stop();
})();
