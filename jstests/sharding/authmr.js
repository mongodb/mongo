// Verify that a user with read and write access to database "test" cannot access database "test2"
// via a mapper, reducer or finalizer.

//
// User document declarations.  All users in this test are added to the admin database.
//

var adminUser = {
    user: "admin",
    pwd: "a",
    roles: [ "readWriteAnyDatabase",
             "dbAdminAnyDatabase",
             "userAdminAnyDatabase",
             "clusterAdmin" ]
}

var test1User = {
    user: "test",
    pwd: "a",
    roles: [{name: 'readWrite', source: 'test1', hasRole: true, canDelegate: false}]
};

function assertGLEOK(status) {
    assert(status.ok && status.err === null,
           "Expected OK status object; found " + tojson(status));
}

function assertRemove(collection, pattern) {
    collection.remove(pattern);
    assertGLEOK(collection.getDB().getLastErrorObj());
}

function assertInsert(collection, obj) {
    collection.insert(obj);
    assertGLEOK(collection.getDB().getLastErrorObj());
}

var cluster = new ShardingTest("authwhere", 1, 0, 1,
                               { extraOptions: { keyFile: "jstests/libs/key1" } });

// Set up the test data.
(function() {
    var adminDB = cluster.getDB('admin');
    var test1DB = adminDB.getSiblingDB('test1');
    var test2DB = adminDB.getSiblingDB('test2');
    var ex;
    try {
        adminDB.addUser(adminUser)
        assert(adminDB.auth(adminUser.user, adminUser.pwd));

        adminDB.removeUser(test1User.user);
        adminDB.addUser(test1User);

        assertInsert(test1DB.foo, { a: 1 });
        assertInsert(test1DB.foo, { a: 2 });
        assertInsert(test1DB.foo, { a: 3 });
        assertInsert(test1DB.foo, { a: 4 });
        assertInsert(test2DB.foo, { x: 1 });
    }
    finally {
        adminDB.logout();
    }
}());

assert.throws(function() {
    var adminDB = cluster.getDB('admin');
    var test1DB;
    var test2DB;
    assert(adminDB.auth(test1User.user, test1User.pwd));
    try {
        test1DB = adminDB.getSiblingDB("test1");
        test2DB = adminDB.getSiblingDB("test2");

        // Sanity check.  test1User can count (read) test1, but not test2.
        assert.eq(test1DB.foo.count(), 4);
        assert.throws(test2DB.foo.count);

        test1DB.foo.mapReduce(
            function () {
                emit(0, this.a);
                var t2 = new Mongo().getDB("test2");
                t2.ad.insert(this);
            },
            function (k, vs) {
                var t2 = new Mongo().getDB("test2");
                t2.reductio.insert(this);

                return Array.sum(vs);
            },
            { out: "bar",
              finalize: function (k, v) {
                  for (k in this) {
                      if (this.hasOwnProperty(k))
                          print(k + "=" + v);
                  }
                  var t2 = new Mongo().getDB("test2");
                  t2.absurdum.insert({ key: k, value: v });
              }
            });
    }
    finally {
        adminDB.logout();
    }
});

(function() {
    var adminDB = cluster.getDB('admin');
    assert(adminDB.auth(adminUser.user, adminUser.pwd));
    try {
        var test2DB = cluster.getDB('test2');
        assert.eq(test2DB.reductio.count(), 0, "reductio");
        assert.eq(test2DB.ad.count(), 0, "ad");
        assert.eq(test2DB.absurdum.count(), 0, "absurdum");
    }
    finally {
        adminDB.logout();
    }
}());
