function setupReplSet() {
    const rst = new ReplSetTest({
        nodes: 1,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagSecurityToken: true,
                testOnlyValidatedTenancyScopeKey: 'secret',
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    let primary = rst.getPrimary();
    let adminDb = primary.getDB('admin');
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));
    return rst;
}

function createAndSetSecurityToken(conn, tenantId, bExpectPrefix) {
    if (typeof conn._securityToken == 'undefined') {
        const tenantToken = _createTenantToken({tenant: tenantId, expectPrefix: bExpectPrefix});
        conn._setSecurityToken(tenantToken);
    }
}

function insertDb(conn, dbName) {
    const db = conn.getDB(dbName);
    assert.commandWorked(db.runCommand({insert: 'some_collection', documents: [{_id: 0}]}));
}

function checkDbNum(conn, dbNum) {
    const db = conn.getDB("admin");
    let listDb = assert.commandWorked(db.adminCommand({listDatabases: 1, nameOnly: true}));
    assert.eq(listDb.databases.length, dbNum, tojson(listDb));
}

function resetSecurityToken(conn) {
    conn._setSecurityToken(undefined);
}

function runTests() {
    let rst = setupReplSet();
    let primary = rst.getPrimary();

    const tenant = ObjectId();
    const tenant2 = ObjectId();
    const tenant3 = ObjectId();

    {
        const conn = Mongo(primary.host);
        assert(conn.getDB("admin").auth('admin', 'pwd'));
        createAndSetSecurityToken(conn, tenant, true);
        insertDb(conn, tenant + "_firstRegDb");
        checkDbNum(conn, 1);
        resetSecurityToken(conn)
    }

    createAndSetSecurityToken(primary, tenant2, false);
    insertDb(primary, "secondRegDb");
    insertDb(primary, "thirdRegDb");
    checkDbNum(primary, 2);

    resetSecurityToken(primary)

    createAndSetSecurityToken(primary, tenant3, false);
    insertDb(primary, "fourthRegDb");
    checkDbNum(primary, 1);

    resetSecurityToken(primary)

    createAndSetSecurityToken(primary, tenant2, false);
    insertDb(primary, "fifthRegDb");
    checkDbNum(primary, 3);

    rst.stopSet();
}

function runTestExpectPrefixTrue() {
    let rst = setupReplSet();
    let primary = rst.getPrimary();

    const tenant = ObjectId();
    const tenant2 = ObjectId();
    const tenant3 = ObjectId();

    createAndSetSecurityToken(primary, tenant, true);
    insertDb(primary, tenant + "_firstRegDb");
    checkDbNum(primary, 1);
    resetSecurityToken(primary)

    createAndSetSecurityToken(primary, tenant2, true);
    insertDb(primary, tenant2 + "_secondRegDb");
    insertDb(primary, tenant2 + "_thirdRegDb");
    checkDbNum(primary, 2);

    // will fail if prefix not provided on insert
    const nonPrefixDb = primary.getDB("_fourthRegDb");
    assert.commandFailedWithCode(
        nonPrefixDb.runCommand({insert: 'some_collection', documents: [{_id: 0}]}), 8423386);

    rst.stopSet();
}

runTests();
runTestExpectPrefixTrue();
