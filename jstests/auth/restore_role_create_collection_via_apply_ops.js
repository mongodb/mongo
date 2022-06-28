// Verify that mongorestore can create a collection via applyOps

(function() {
'use strict';

function makeCreateOp(collName, uuid = undefined) {
    const op = {
        op: 'c',
        ns: 'test.$cmd',
        o: {
            create: collName,
            idIndex: {
                key: {_id: 1},
                v: 2,
                name: "_id_",
                ns: "test." + collName,
            },
        },
    };
    if (uuid) {
        op.ui = uuid;
    }
    return op;
}

function assertHasCollection(db, collName, expectUUID = undefined) {
    const colls = db.getCollectionInfos({name: collName});
    assert.eq(colls.length, 1, colls);
    if (expectUUID !== undefined) {
        assert.eq(colls[0].info.uuid, expectUUID, colls);
    }
}

function runTest(conn) {
    const admin = conn.getDB('admin');
    const test = conn.getDB('test');
    assert.commandWorked(admin.runCommand({createUser: 'admin', pwd: 'admin', roles: ['root']}));
    assert(admin.auth('admin', 'admin'));

    assert.commandWorked(
        admin.runCommand({createUser: 'restore1', pwd: 'pwd', roles: ['restore']}));
    admin.logout();

    assert(admin.auth('restore1', 'pwd'));

    // Simple create collection op.
    assert.commandWorked(admin.runCommand({applyOps: [makeCreateOp('test1')]}));
    assertHasCollection(test, 'test1');

    // Create collection with UUID.
    const kSpecificUUID = UUID();
    assert.commandWorked(admin.runCommand({applyOps: [makeCreateOp('test2', kSpecificUUID)]}));
    assertHasCollection(test, 'test2', kSpecificUUID);

    admin.logout();
}

const standalone = MongoRunner.runMongod({auth: ''});
runTest(standalone);
MongoRunner.stopMongod(standalone);
})();