const tenant = '636d957b2646ddfaf9b5e13f';
const kVTSKey = 'secret';

const insertCmd = {
    insert: 'some_collection',
    documents: [{_id: 0}]
};

function createnewReplSetTest(param) {
    const rst = new ReplSetTest({nodes: 1, nodeOptions: {auth: '', setParameter: param}});
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();
    return rst;
}

function setupNewReplSetWithParam(param) {
    let rst = createnewReplSetTest(param)
    let primary = rst.getPrimary();
    let adminDb = primary.getDB('admin');
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));
    return rst;
}

function createAndSetSecurityToken(primary) {
    const kTenantID = ObjectId(tenant);
    if (typeof primary._securityToken == 'undefined') {
        const tenantToken = _createTenantToken({tenant: kTenantID, expectPrefix: true});
        primary._setSecurityToken(tenantToken);
    }
}

function securityTokenMissing(primary) {
    const db = primary.getDB(tenant + "_SecTokenMissing");
    assert.commandFailedWithCode(db.runCommand(insertCmd), 8233503);
}

function tenantPrefixMissing(primary) {
    const db = primary.getDB("TenantPrefixMissing");
    assert.commandFailedWithCode(db.runCommand(insertCmd), 8423386);
}

function unmatchedTenantInfo(primary) {
    const unmatchingTenant = '636d957b2646ddfaf9b5e13a';
    const db = primary.getDB(unmatchingTenant + "_UnmatchedTenantInfo");
    assert.commandFailedWithCode(db.runCommand(insertCmd), 8423384);
}

function invalidTenantPrefix(primary) {
    const invalidTenant = '636d95';
    const db = primary.getDB(invalidTenant + "_InvalidTenantPrefix");
    assert.commandFailedWithCode(db.runCommand(insertCmd), 8423386);
}

function tenantMustBeSet(primary) {
    const db = primary.getDB("tenantMustBeSet");
    assert.commandFailedWithCode(db.runCommand(insertCmd), 8423388);
}

function runTests() {
    let rst = setupNewReplSetWithParam({
        multitenancySupport: true,
        featureFlagSecurityToken: true,
        testOnlyValidatedTenancyScopeKey: kVTSKey,
    });

    let primary = rst.getPrimary();

    securityTokenMissing(primary);

    createAndSetSecurityToken(primary);

    tenantPrefixMissing(primary);
    unmatchedTenantInfo(primary);
    invalidTenantPrefix(primary);
    rst.stopSet();

    rst = setupNewReplSetWithParam({
        multitenancySupport: true,
    });
    primary = rst.getPrimary();

    tenantMustBeSet(primary);
    rst.stopSet();
}

runTests();
