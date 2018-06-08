// Auth tests for the listDatabases command.

(function() {
    'use strict';

    const mongod = MongoRunner.runMongod({auth: ""});
    const admin = mongod.getDB('admin');

    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(admin.auth('admin', 'pass'));

    // Establish db0..db7
    for (let i = 0; i < 8; ++i) {
        mongod.getDB('db' + i).foo.insert({bar: "baz"});
    }

    // Make db0, db2, db4, db6 readable to user1 abd user3.
    // Make db0, db1, db2, db3 read/writable to user 2 and user3.
    function makeRole(perm, dbNum) {
        return {role: perm, db: ("db" + dbNum)};
    }
    const readEven = [0, 2, 4, 6].map(function(i) {
        return makeRole("read", i);
    });
    const readWriteLow = [0, 1, 2, 3].map(function(i) {
        return makeRole("readWrite", i);
    });
    admin.createUser({user: 'user1', pwd: 'pass', roles: readEven});
    admin.createUser({user: 'user2', pwd: 'pass', roles: readWriteLow});
    admin.createUser({user: 'user3', pwd: 'pass', roles: readEven.concat(readWriteLow)});
    admin.logout();

    var admin_dbs = ["admin", "db0", "db1", "db2", "db3", "db4", "db5", "db6", "db7"];
    // mobile storage engine might not have a local database
    if (jsTest.options().storageEngine !== "mobile") {
        admin_dbs.push("local");
    }

    [{user: "user1", dbs: ["db0", "db2", "db4", "db6"]},
     {user: "user2", dbs: ["db0", "db1", "db2", "db3"]},
     {user: "user3", dbs: ["db0", "db1", "db2", "db3", "db4", "db6"]},
     {user: "admin", dbs: admin_dbs},
    ].forEach(function(test) {
        admin.auth(test.user, 'pass');
        const dbs = assert.commandWorked(admin.runCommand({listDatabases: 1}));
        assert.eq(dbs.databases
                      .map(function(db) {
                          return db.name;
                      })
                      .sort(),
                  test.dbs,
                  test.user + " permissions");
        admin.logout();
    });

    MongoRunner.stopMongod(mongod);
})();
