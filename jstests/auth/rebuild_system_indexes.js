/**
 * This test verifies that the server will attempt to rebuild admin.system.users index on startup if
 * it is in a partially built state using the mmapv1 storage engine.
 *
 * @tags: [requires_journaling, requires_mmapv1]
 */
(() => {
    "use strict";

    const dbpath = "rebuild_system_indexes";
    const user = 'admin';
    const pwd = 'adminPassword';

    const mongod = MongoRunner.runMongod({storageEngine: 'mmapv1', dbpath});

    let admin = mongod.getDB('admin');

    // Create the system.users index
    assert.commandWorked(
        admin.runCommand({createUser: user, pwd, roles: [{role: 'root', db: 'admin'}]}),
        'failed to create user/indexes');
    assert.commandWorked(admin.runCommand({deleteIndexes: 'system.users', index: '*'}),
                         'failed to drop indexes');

    MongoRunner.stopMongod(mongod);

    // Add a fail point so the server crashes while rebuilding the indexes
    print('================= starting mongod that WILL CRASH PURPOSEFULLY =================');
    MongoRunner.runMongod({
        restart: mongod,
        storageEngine: 'mmapv1',
        setParameter: "failpoint.crashAfterStartingIndexBuild={mode: 'alwaysOn'}", dbpath,
    });
    print('======================== END PURPOSEFUL CRASH ========================');

    // Make sure the server isnt running
    assert.neq(0,
               runMongoProgram('mongo', '--port', mongod.port, '--eval', 'db.isMaster()'),
               "mongod did not crash when creating index");

    // Start up normally, indexes should be rebuilt
    MongoRunner.runMongod({
        restart: mongod,
        storageEngine: 'mmapv1',
        setParameter: "failpoint.crashAfterStartingIndexBuild={mode: 'off'}", dbpath,
    });

    assert(mongod);

    admin = mongod.getDB('admin');
    admin.auth(user, pwd);

    assert(admin.getCollection('system.users').getIndexes().length > 1, 'indexes were not rebuilt');

    MongoRunner.stopMongod(mongod);
})();
