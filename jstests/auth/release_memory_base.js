// Test that only users with releaseMemoryAnyCursor permission can use releaseMemory command

const db1Name = "release_memory_base_1";
const db2Name = "release_memory_base_2";
const coll1Name = "coll1";
const coll2Name = "coll2";

function setupCollections(conn) {
    const db1 = conn.getDB(db1Name);
    const db2 = conn.getDB(db2Name);
    db1.dropDatabase();
    db2.dropDatabase();
    db1[coll1Name].insertMany([{}, {}, {}]);
    db1[coll2Name].insertMany([{}, {}, {}]);
    db2[coll1Name].insertMany([{}, {}, {}]);
}

function assertCorrectResult(res, shouldWork, user) {
    if (shouldWork) {
        assert.commandWorked(res, "user: " + user);
    } else {
        assert.commandFailedWithCode(res, [ErrorCodes.Unauthorized], "user: " + user);
    }
}

const createInactiveCursor = function(db, collName) {
    const cmdRes = db.runCommand({find: collName, filter: {}, batchSize: 1});
    assert.commandWorked(cmdRes);
    return cmdRes.cursor.id;
};

function testReleaseMemory(
    conn, user, shouldWorkCollection, shouldWorkDatabase, shouldWorkCluster) {
    const admin = conn.getDB('admin');
    admin.auth(user, "pass");

    const db1 = conn.getDB(db1Name);
    const db2 = conn.getDB(db2Name);

    assertCorrectResult(db1.runCommand({releaseMemory: [createInactiveCursor(db1, coll1Name)]}),
                        shouldWorkCollection,
                        user);
    assertCorrectResult(db1.runCommand({releaseMemory: [createInactiveCursor(db1, coll2Name)]}),
                        shouldWorkDatabase,
                        user);
    assertCorrectResult(db2.runCommand({releaseMemory: [createInactiveCursor(db2, coll1Name)]}),
                        shouldWorkCluster,
                        user);

    admin.logout();
}

export function runTest(conn) {
    const admin = conn.getDB('admin');
    admin.createUser({user: 'admin', pwd: 'pass', roles: jsTest.adminUserRoles});

    assert(admin.auth('admin', 'pass'));
    setupCollections(conn);

    const db1 = conn.getDB(db1Name);
    assert.commandFailedWithCode(
        db1.runCommand({releaseMemory: [createInactiveCursor(db1, coll1Name)]}),
        [ErrorCodes.Unauthorized],
        "releaseMemory should not work without releaseMemoryAnyCursor permission");

    admin.createRole({
        createRole: "releaseCollection",
        roles: ["readAnyDatabase"],
        privileges: [{
            resource: {db: "release_memory_base_1", collection: "coll1"},
            actions: ["releaseMemoryAnyCursor"],
        }],
    })
    admin.createRole({
        createRole: "releaseDatabase",
        roles: ["readAnyDatabase"],
        privileges: [{
            resource: {db: "release_memory_base_1", collection: ""},
            actions: ["releaseMemoryAnyCursor"],
        }],
    })
    admin.createRole({
        createRole: "releaseCluster",
        roles: ["readAnyDatabase"],
        privileges: [{resource: {cluster: true}, actions: ["releaseMemoryAnyCursor"]}],
    })

    admin.createUser({user: "userCollection", pwd: "pass", roles: ["releaseCollection"]});
    admin.createUser({user: "userDatabase", pwd: "pass", roles: ["releaseDatabase"]});
    admin.createUser({user: "userCluster", pwd: "pass", roles: ["releaseCluster"]});

    admin.logout();

    testReleaseMemory(conn, "userCollection", true, false, false);
    testReleaseMemory(conn, "userDatabase", true, true, false);
    testReleaseMemory(conn, "userCluster", true, true, true);
}
