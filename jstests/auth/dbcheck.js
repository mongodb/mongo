/**
 * Validate dbCheck authorization.
 */

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

const m = MongoRunner.runMongod({auth: ""});

const db = m.getDB("test");
const admin = m.getDB('admin');

// Set up users and add initial data to be dbChecked later.
{
    admin.createUser({user: 'root', pwd: 'password', roles: [{role: "root", db: "admin"}]});
    admin.auth('root', 'password');
    admin.createUser({
        user: 'userReadWriteAnyDatabase',
        pwd: 'pwdReadWriteAnyDatabase',
        roles: [{role: "readWriteAnyDatabase", db: "admin"}]
    });
    admin.createUser({
        user: 'userClusterManager',
        pwd: 'pwdClusterManager',
        roles: [{role: "clusterManager", db: "admin"}]
    });
    assert.commandWorked(db.c.insertOne({}));
    admin.logout();
}
// Validate that a non-clusterManager user cannot run dbCheck.
{
    assert(admin.auth('userReadWriteAnyDatabase', 'pwdReadWriteAnyDatabase'));
    assert.commandWorked(db.c.insertOne({}));
    assert.commandFailedWithCode(db.runCommand({dbCheck: 1}), ErrorCodes.Unauthorized);
    admin.logout();
}
// Validate that clusterManager can run dbCheck and inspect its output.
{
    assert(admin.auth('userClusterManager', 'pwdClusterManager'));
    const local = db.getSiblingDB('local');
    assert.eq(0, local.system.healthlog.find().itcount());
    assert.commandWorked(db.runCommand({dbCheck: 1}));

    // Wait for the dbCheck deferred writer to populate the healthlog. See SERVER-61765.
    assert.soon(() => local.system.healthlog.find().itcount() > 0);
    admin.logout();
}

MongoRunner.stopMongod(m, null, {user: 'root', pwd: 'password'});
