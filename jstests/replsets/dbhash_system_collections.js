// Test that the dbhash command checks system collections that are replicated.
'use strict';

(function() {
    var rst = new ReplSetTest({name: 'dbhash_system_collections', nodes: 2});
    rst.startSet();
    rst.initiate();

    var primary = rst.getPrimary();
    var secondary = rst.getSecondary();

    var testDB = primary.getDB('test');
    assert.writeOK(testDB.system.users.insert({users: 1}));
    assert.writeOK(testDB.system.js.insert({js: 1}));

    var adminDB = primary.getDB('admin');
    assert.writeOK(adminDB.system.roles.insert({roles: 1}));
    assert.writeOK(adminDB.system.version.insert({version: 1}));
    assert.writeOK(adminDB.system.new_users.insert({new_users: 1}));
    assert.writeOK(adminDB.system.backup_users.insert({backup_users: 1}));

    rst.awaitReplication();

    function checkDbHash(mongo) {
        var testDB = mongo.getDB('test');
        var adminDB = mongo.getDB('admin');

        var replicatedSystemCollections = [
            'system.js',
            'system.users',
        ];

        var replicatedAdminSystemCollections = [
            'system.backup_users',
            'system.new_users',
            'system.roles',
            'system.version',
        ];

        var res = testDB.runCommand('dbhash');
        assert.commandWorked(res);
        assert.docEq(Object.keys(res.collections), replicatedSystemCollections, tojson(res));

        res = adminDB.runCommand('dbhash');
        assert.commandWorked(res);
        assert.docEq(Object.keys(res.collections), replicatedAdminSystemCollections, tojson(res));

        return res.md5;
    }

    var primaryMd5 = checkDbHash(primary);
    var secondaryMd5 = checkDbHash(secondary);
    assert.eq(primaryMd5, secondaryMd5, 'dbhash is different on the primary and the secondary');
})();
