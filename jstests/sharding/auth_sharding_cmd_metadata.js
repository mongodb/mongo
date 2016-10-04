/**
 * Tests that only the internal user will be able to advance the config server opTime.
 */
(function() {

    "use strict";

    var st = new ShardingTest({shards: 1, other: {keyFile: 'jstests/libs/key1'}});

    var adminUser = {db: "admin", username: "foo", password: "bar"};

    st.s.getDB(adminUser.db).createUser({user: 'foo', pwd: 'bar', roles: jsTest.adminUserRoles});

    st.s.getDB('admin').auth('foo', 'bar');

    st.adminCommand({enableSharding: 'test'});
    st.adminCommand({shardCollection: 'test.user', key: {x: 1}});

    st.d0.getDB('admin').createUser({user: 'user', pwd: 'pwd', roles: jsTest.adminUserRoles});
    st.d0.getDB('admin').auth('user', 'pwd');

    var maxSecs = Math.pow(2, 32) - 1;
    var metadata = {configsvr: {opTime: {ts: Timestamp(maxSecs, 0), t: maxSecs}}};
    var res = st.d0.getDB('test').runCommandWithMetadata("ping", {ping: 1}, metadata);

    assert.commandFailedWithCode(res.commandReply, ErrorCodes.Unauthorized);

    // Make sure that the config server optime did not advance.
    var status = st.d0.getDB('test').runCommand({serverStatus: 1});
    assert.neq(null, status.sharding);
    assert.lt(status.sharding.lastSeenConfigServerOpTime.t, maxSecs);

    st.d0.getDB('admin').createUser({user: 'internal', pwd: 'pwd', roles: ['__system']});
    st.d0.getDB('admin').auth('internal', 'pwd');

    res = st.d0.getDB('test').runCommandWithMetadata("ping", {ping: 1}, metadata);
    assert.commandWorked(res.commandReply);

    status = st.d0.getDB('test').runCommand({serverStatus: 1});
    assert.neq(null, status.sharding);
    assert.eq(status.sharding.lastSeenConfigServerOpTime.t, maxSecs);

    st.stop();

})();
