/** Ensure that authorization system collections' indexes are correctly generated.
 *
 * This test requires users to persist across a restart.
 * @tags: [requires_persistence]
 */

(function() {
    let conn = MongoRunner.runMongod({smallfiles: ""});
    let db = conn.getDB("admin");

    // TEST: User and role collections start off with no indexes
    assert.eq(0, db.system.users.getIndexes().length);
    assert.eq(0, db.system.roles.getIndexes().length);

    // TEST: User and role creation generates indexes
    db.createUser({user: "user", pwd: "pwd", roles: []});
    assert.eq(2, db.system.users.getIndexes().length);

    db.createRole({role: "role", privileges: [], roles: []});
    assert.eq(2, db.system.roles.getIndexes().length);

    // TEST: Destroying admin.system.users index and restarting will recreate it
    assert.commandWorked(db.system.users.dropIndexes());
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn, cleanData: false});
    db = conn.getDB("admin");
    assert.eq(2, db.system.users.getIndexes().length);
    assert.eq(2, db.system.roles.getIndexes().length);

    // TEST: Destroying admin.system.roles index and restarting will recreate it
    assert.commandWorked(db.system.roles.dropIndexes());
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn, cleanData: false});
    db = conn.getDB("admin");
    assert.eq(2, db.system.users.getIndexes().length);
    assert.eq(2, db.system.roles.getIndexes().length);

    // TEST: Destroying both authorization indexes and restarting will recreate them
    assert.commandWorked(db.system.users.dropIndexes());
    assert.commandWorked(db.system.roles.dropIndexes());
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn, cleanData: false});
    db = conn.getDB("admin");
    assert.eq(2, db.system.users.getIndexes().length);
    assert.eq(2, db.system.roles.getIndexes().length);

    // TEST: Destroying the admin.system.users index and restarting will recreate it, even if
    // admin.system.roles does not exist
    db.dropDatabase();
    db.createUser({user: "user", pwd: "pwd", roles: []});
    assert.commandWorked(db.system.users.dropIndexes());
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn, cleanData: false});
    db = conn.getDB("admin");
    assert.eq(2, db.system.users.getIndexes().length);

    // TEST: Destroying the admin.system.roles index and restarting will recreate it, even if
    // admin.system.users does not exist
    db.dropDatabase();
    db.createRole({role: "role", privileges: [], roles: []});
    assert.commandWorked(db.system.roles.dropIndexes());
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: conn, cleanData: false});
    db = conn.getDB("admin");
    assert.eq(2, db.system.roles.getIndexes().length);
})();
