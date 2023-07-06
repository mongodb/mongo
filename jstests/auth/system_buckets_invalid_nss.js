// Validate that *.system.buckets.system.buckets.* is an invalid namespace

function runTest(conn) {
    const admin = conn.getDB('admin');
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));

    assert.commandFailedWithCode(admin.system.buckets.system.buckets.foo.insert({x: 1}),
                                 [ErrorCodes.Unauthorized]);

    assert(admin.auth('admin', 'admin'));
    assert.commandFailedWithCode(admin.system.buckets.system.buckets.foo.insert({x: 1}),
                                 [ErrorCodes.InvalidNamespace]);
}

const mongod = MongoRunner.runMongod({auth: ''});
runTest(mongod);
MongoRunner.stopMongod(mongod);