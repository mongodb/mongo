// Authenticate to a merizod from the shell via command line.

(function() {
    'use strict';

    const port = allocatePort();
    const merizod = MongoRunner.runMongod({auth: '', port: port});
    const admin = merizod.getDB('admin');

    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});

    // Connect via shell round-trip in order to verify handling of merizodb:// uri with password.
    const uri = 'merizodb://admin:pass@localhost:' + port + '/admin';
    // Be sure to actually do something requiring authentication.
    const merizo = runMongoProgram('merizo', uri, '--eval', 'db.system.users.find({});');
    assert.eq(merizo, 0, "Failed connecting to merizod via shell+merizodb uri");

    MongoRunner.stopMongod(merizod);
})();
