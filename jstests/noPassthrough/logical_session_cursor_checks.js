// @tags: [
//   requires_sharding,
// ]

(function() {
'use strict';

function runFixture(Fixture) {
    var fixture = new Fixture();
    var conn = fixture.getConn();
    var admin = conn.getDB("admin");
    var data = conn.getDB("data_storage");

    admin.createUser({user: 'admin', pwd: 'admin', roles: jsTest.adminUserRoles});
    admin.auth("admin", "admin");
    data.createUser({user: 'admin', pwd: 'admin', roles: jsTest.basicUserRoles});
    data.createUser({user: 'user0', pwd: 'password', roles: jsTest.basicUserRoles});
    admin.logout();

    data.auth("user0", "password");
    assert.commandWorked(data.test.insert({name: "first", data: 1}));
    assert.commandWorked(data.test.insert({name: "second", data: 2}));

    // Test that getMore works correctly on the same session.
    {
        var session1 = conn.startSession();
        var session2 = conn.startSession();
        var res = assert.commandWorked(
            session1.getDatabase("data_storage").runCommand({find: "test", batchSize: 0}));
        var cursorId = res.cursor.id;
        assert.commandWorked(session1.getDatabase("data_storage")
                                 .runCommand({getMore: cursorId, collection: "test"}));

        session2.endSession();
        session1.endSession();
    }

    // Test that getMore correctly gives an error, when using a cursor on a different session.
    {
        var session1 = conn.startSession();
        var session2 = conn.startSession();
        var res = assert.commandWorked(
            session1.getDatabase("data_storage").runCommand({find: "test", batchSize: 0}));
        var cursorId = res.cursor.id;
        assert.commandFailed(session2.getDatabase("data_storage")
                                 .runCommand({getMore: cursorId, collection: "test"}));

        session2.endSession();
        session1.endSession();
    }

    // Test that query.js driven getMore works correctly on the same session.
    {
        var session1 = conn.startSession();
        var session2 = conn.startSession();
        var cursor = session1.getDatabase("data_storage").test.find({}).batchSize(1);
        cursor.next();
        cursor.next();
        cursor.close();

        session2.endSession();
        session1.endSession();
    }

    fixture.stop();
}

function Standalone() {
    this.standalone = MongoRunner.runMongod({auth: ""});
}

Standalone.prototype.stop = function() {
    MongoRunner.stopMongod(this.standalone);
};

Standalone.prototype.getConn = function() {
    return this.standalone;
};

function Sharding() {
    this.st =
        new ShardingTest({shards: 1, config: 1, mongos: 1, other: {keyFile: 'jstests/libs/key1'}});
}

Sharding.prototype.stop = function() {
    this.st.stop();
};

Sharding.prototype.getConn = function() {
    return this.st.s0;
};

[Standalone, Sharding].forEach(runFixture);
})();
