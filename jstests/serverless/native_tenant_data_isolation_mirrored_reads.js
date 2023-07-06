/**
 * Test mirrored reads in a multi-tenant environment.
 */

const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {
        auth: '',
        setParameter: {
            multitenancySupport: true,
            logComponentVerbosity: tojson({command: 1}),
            mirrorReads: tojsononeline({samplingRate: 1.0}),
            "failpoint.mirrorMaestroExpectsResponse": tojson({mode: "alwaysOn"}),
        }
    }
});
rst.startSet({keyFile: 'jstests/libs/key1'});
rst.initiate();

const primary = rst.getPrimary();
const adminDb = primary.getDB('admin');
const secondary = rst.getSecondary();

// Prepare a user for testing of passing the tenant using $tenant.
// Must be authenticated as a user with ActionType::useTenant in order to use $tenant.
assert.commandWorked(adminDb.runCommand({createUser: 'admin', pwd: 'pwd', roles: ['root']}));
assert(adminDb.auth('admin', 'pwd'));
assert(secondary.getDB('admin').auth('admin', 'pwd'));

const kTenant = ObjectId();
const kOtherTenant = ObjectId();
const kDbName = 'myDb';
const kCollName = 'myColl';
const testDb = primary.getDB(kDbName);

function getMirroredReadsStats(node) {
    return node.getDB(kDbName).serverStatus({mirroredReads: 1}).mirroredReads;
}

function assertSecondaryStats(initialSecondaryStats, numSentSince) {
    const currentSecondaryStats = getMirroredReadsStats(secondary);
    jsTestLog("Current secondary stats: " + tojson(currentSecondaryStats));
    const numProcessed = currentSecondaryStats.processedAsSecondary;
    return initialSecondaryStats.processedAsSecondary + numSentSince == numProcessed;
}

const kTenantDocs = [{w: 0}, {x: 1}, {y: 2}, {z: 3}];
const kOtherTenantDocs = [{i: 1}, {j: 2}, {k: 3}];

assert.commandWorked(
    testDb.runCommand({insert: kCollName, documents: kTenantDocs, '$tenant': kTenant}));
assert.commandWorked(
    testDb.runCommand({insert: kCollName, documents: kOtherTenantDocs, '$tenant': kOtherTenant}));

function verifyMirroredReadStats(cmd) {
    const initialPrimaryStats = getMirroredReadsStats(primary);
    const initialSecondaryStats = getMirroredReadsStats(secondary);
    jsTestLog("Verifying mirrored reads for cmd: " + tojson(cmd));
    jsTestLog("Initial primary stats: " + tojson(initialPrimaryStats));
    jsTestLog("Initial secondary stats: " + tojson(initialSecondaryStats));

    // Check that the mirrored operation is observable through the metrics.
    assert.commandWorked(testDb.runCommand(cmd));
    let currentPrimaryStats;
    assert.soon(() => {
        currentPrimaryStats = getMirroredReadsStats(primary);
        jsTestLog("Current primary stats: " + tojson(currentPrimaryStats));
        let resolved = currentPrimaryStats.resolved;
        let succeeded = currentPrimaryStats.succeeded;
        return (initialPrimaryStats.resolved + 1 == resolved) &&
            (initialPrimaryStats.succeeded + 1 == succeeded);
    });
    assert.eq(initialPrimaryStats.seen + 1, currentPrimaryStats.seen, currentPrimaryStats);
    assertSecondaryStats(initialSecondaryStats, 1);
}

// Verify that mirrored reads are successful for mirrored operations with '$tenant'.
verifyMirroredReadStats({find: kCollName, projection: {_id: 0}, '$tenant': kTenant});
verifyMirroredReadStats({find: kCollName, projection: {_id: 0}, '$tenant': kOtherTenant});

verifyMirroredReadStats({count: kCollName, query: {x: 1}, '$tenant': kTenant});
verifyMirroredReadStats({count: kCollName, query: {i: 1}, '$tenant': kOtherTenant});

verifyMirroredReadStats({distinct: kCollName, key: 'x', '$tenant': kTenant});
verifyMirroredReadStats({distinct: kCollName, key: 'i', '$tenant': kOtherTenant});

verifyMirroredReadStats(
    {findAndModify: kCollName, query: {x: 1}, update: {$inc: {x: 10}}, '$tenant': kTenant});
verifyMirroredReadStats(
    {findAndModify: kCollName, query: {i: 1}, update: {$inc: {i: 10}}, '$tenant': kOtherTenant});

verifyMirroredReadStats({
    update: kCollName,
    updates: [{q: {x: 1}, u: {'inc': {x: 1}}}],
    ordered: false,
    '$tenant': kTenant
});
verifyMirroredReadStats({
    update: kCollName,
    updates: [{q: {i: 1}, u: {'inc': {i: 1}}}],
    ordered: false,
    '$tenant': kOtherTenant
});

rst.stopSet();