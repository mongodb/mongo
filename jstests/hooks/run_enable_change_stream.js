const dbName = "admin";
const conn = db.getMongo();

function createAndSetSecurityToken(kTenantId) {
    if (typeof conn._securityToken == 'undefined' && TestData.hasOwnProperty("tenantId")) {
        print(
            `set security token to the connection: "${tojsononeline(conn)}", tenant: ${kTenantId}`);
        const tenantToken = _createTenantToken({tenant: kTenantId});
        conn._setSecurityToken(tenantToken);
    }
}

function runCommandWithResponseCheck() {
    const db = conn.getDB(dbName);
    assert.commandWorked(db.runCommand({setChangeStreamState: 1, enabled: true}));
    const changeStreamObj = assert.commandWorked(db.runCommand({getChangeStreamState: 1}));
    assert.eq(changeStreamObj.enabled, true);
}

if (TestData.hasOwnProperty("tenantId")) {
    let kTenantId = ObjectId(TestData.tenantId);
    createAndSetSecurityToken(kTenantId);
}
runCommandWithResponseCheck();
