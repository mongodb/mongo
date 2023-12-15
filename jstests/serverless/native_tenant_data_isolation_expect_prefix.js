// Test combinations of tenantId sources in a serverless multitenant environment, including from
// database string prefixes and security token.  Combinations including prefixes will
// require the use of the expectPrefix field.

import {arrayEq} from "jstests/aggregation/extras/utils.js";

const kTenant = ObjectId();
const kTestDb = 'testDb0';
const kCollName = 'myColl0';
const kViewName = 'myView0';
const kVTSKey = 'secret';

function checkNsSerializedCorrectly(kDbName, kCollectionName, nsField, options) {
    options = options || {};
    const prefixed = options.prefixed || false;
    const tenantId = options.tenantId || "";

    const targetNss = (prefixed ? tenantId + "_" : "") + kDbName +
        (kCollectionName == "" ? "" : "." + kCollectionName);
    assert.eq(nsField, targetNss);
}

function checkFindCommandPasses(request, targetDb, targetDoc, options) {
    options = options || {};
    const prefixed = options.prefixed || false;

    const findRes = assert.commandWorked(targetDb.runCommand(request));
    assert(arrayEq([targetDoc], findRes.cursor.firstBatch), tojson(findRes));
    checkNsSerializedCorrectly(
        kTestDb, kCollName, findRes.cursor.ns, {tenantId: kTenant, prefixed: prefixed});
}

function checkFindCommandFails(request, targetDb, options) {
    options = options || {};
    const prefixed = options.prefixed || false;

    const findRes = assert.commandWorked(targetDb.runCommand(request));
    assert(arrayEq([], findRes.cursor.firstBatch), tojson(findRes));
    checkNsSerializedCorrectly(
        kTestDb, kCollName, findRes.cursor.ns, {tenantId: kTenant, prefixed: prefixed});
}

function checkDbStatsCommand(request, targetDb, {targetPass}) {
    let statsRes = assert.commandWorked(targetDb.runCommand(request));
    assert.eq(statsRes.collections, targetPass ? 2 : 0, tojson(statsRes));
}

function checkCountCommandPasses(request, targetDb, expectedCount) {
    let countRes = assert.commandWorked(targetDb.runCommand(request));
    assert.eq(countRes.n, expectedCount, tojson(countRes));
}

function checkDistinctCommandPasses(request, targetDb, expectedValues) {
    let distinctRes = assert.commandWorked(targetDb.runCommand(request));
    assert.eq(distinctRes.values, expectedValues, tojson(distinctRes));
}

function checkExplainCommandPasses(request, targetDb, tenantId) {
    let response = assert.commandWorked(targetDb.runCommand(request));
    let nss = "";
    if (response.hasOwnProperty("stages")) {
        nss = response.stages[0].$cursor.queryPlanner.namespace;
    } else {
        nss = response.queryPlanner.namespace;
    }
    assert(nss.startsWith(tenantId + "_"), tojsononeline(response));
}

function createTenantCommand(request, options) {
    const {expectPrefix = false} = options;

    if (expectPrefix) {
        request = Object.assign(request, {'expectPrefix': true});
    }
    return request;
}

function testCountCommand(targetDb, options) {
    let request = createTenantCommand({count: kCollName, query: {}}, options);
    checkCountCommandPasses(request, targetDb, 1);

    let viewRequest = createTenantCommand({count: kViewName, query: {}}, options);
    checkCountCommandPasses(viewRequest, targetDb, 1);

    // explain count command on view.
    let explainRequest =
        createTenantCommand({"explain": {"count": kViewName, "query": {}}}, options);
    checkExplainCommandPasses(explainRequest, targetDb, kTenant);
}

function testDistictCommand(targetDb, options) {
    let expectedValues = [1];
    let request = createTenantCommand({distinct: kCollName, key: "a", query: {}}, options);
    checkDistinctCommandPasses(request, targetDb, expectedValues);

    let viewRequest = createTenantCommand({distinct: kViewName, key: "a", query: {}}, options);
    checkDistinctCommandPasses(viewRequest, targetDb, expectedValues);

    let explainRequest =
        createTenantCommand({explain: {distinct: kViewName, key: "a", query: {}}}, options);
    checkExplainCommandPasses(explainRequest, targetDb, kTenant);
}

function runTestWithSecurityTokenFlag() {
    // setup replSet that uses security token
    const rst = new ReplSetTest({
        nodes: 2,
        nodeOptions: {
            auth: '',
            setParameter: {
                multitenancySupport: true,
                featureFlagSecurityToken: true,
                testOnlyValidatedTenancyScopeKey: kVTSKey,
            }
        }
    });
    rst.startSet({keyFile: 'jstests/libs/key1'});
    rst.initiate();

    const primary = rst.getPrimary();
    const adminDb = primary.getDB('admin');

    // Prepare an authenticated user for testing.
    // Must be authenticated as a user with ActionType::useTenant in order to use security token
    assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
    assert(adminDb.auth('admin', 'pwd'));
    const tokenConn = new Mongo(primary.host);

    const securityToken =
        _createSecurityToken({user: "userTenant1", db: '$external', tenant: kTenant}, kVTSKey);
    const tokenDb = tokenConn.getDB(kTestDb);
    const prefixedTokenDb = tokenConn.getDB(kTenant + '_' + kTestDb);

    // Create a user for kTenant and then set the security token on the connection.
    primary._setSecurityToken(_createTenantToken({tenant: kTenant}));
    assert.commandWorked(primary.getDB('$external').runCommand({
        createUser: "userTenant1",
        roles:
            [{role: 'dbAdminAnyDatabase', db: 'admin'}, {role: 'readWriteAnyDatabase', db: 'admin'}]
    }));
    primary._setSecurityToken(undefined);

    tokenConn._setSecurityToken(securityToken);
    // Logout the root user to avoid multiple authentication.
    tokenConn.getDB("admin").logout();

    // Create a collection by inserting a document to it.
    const testDocs = {_id: 0, a: 1, b: 1};
    assert.commandWorked(prefixedTokenDb.runCommand(
        {insert: kCollName, documents: [testDocs], 'expectPrefix': true}));
    // Create a view.
    assert.commandWorked(prefixedTokenDb.runCommand(
        {create: kViewName, viewOn: kCollName, pipeline: [], expectPrefix: true}));

    // Run a sanity check to locate the collection
    checkDbStatsCommand({dbStats: 1, 'expectPrefix': true}, prefixedTokenDb, {targetPass: true});

    // find with security token using expectPrefix.
    {
        // Baseline sanity check.
        let request = {find: kCollName, filter: {a: 1}};
        checkFindCommandPasses(request, tokenDb, testDocs);

        request = Object.assign(request, {'expectPrefix': false});
        checkFindCommandPasses(request, tokenDb, testDocs);

        // Expect this to fail since we set 'expectPrefix': true without providing a prefix.
        request = Object.assign(request, {'expectPrefix': true});
        assert.commandFailedWithCode(tokenDb.runCommand(request), 8423386);
    }

    // find with security token and prefixed DB using expectPrefix.
    {
        let requestDbStats = {dbStats: 1};

        // By supplying both a security token and a prefixed tenantId without specifying an
        // 'expectPrefix':true field, the tenantId will be applied twice to the namespace or
        // database name, rendering it unresolvable. In this scenario, dbStats will return no
        // locatable collections.
        // Baseline sanity check.
        checkDbStatsCommand(requestDbStats, prefixedTokenDb, {targetPass: false});

        let request = {find: kCollName, filter: {a: 1}};
        checkFindCommandFails(request, prefixedTokenDb, {prefixed: true});

        request = Object.assign(request, {'expectPrefix': false});
        checkFindCommandFails(request, prefixedTokenDb, {prefixed: true});

        request = Object.assign(request, {'expectPrefix': true});
        let findRes = assert.commandWorked(prefixedTokenDb.runCommand(request));

        assert(arrayEq([testDocs], findRes.cursor.firstBatch), tojson(findRes));
        checkNsSerializedCorrectly(
            kTestDb, kCollName, findRes.cursor.ns, {tenantId: kTenant, prefixed: true});
    }

    // count prefix DB with security token.
    testCountCommand(prefixedTokenDb, {expectPrefix: true});

    testDistictCommand(prefixedTokenDb, {expectPrefix: true});

    rst.stopSet();
}

runTestWithSecurityTokenFlag();
