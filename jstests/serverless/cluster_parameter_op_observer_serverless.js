/**
 * Test that ClusterServerParameterOpObserver fires appropriately on serverless replsets.
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_fcv_62,
 *   serverless
 *  ]
 */

(function() {
'use strict';
// For ChangeStreamMultitenantReplicaSetTest.
load("jstests/serverless/libs/change_collection_util.js");

const getTenantConnection = ChangeStreamMultitenantReplicaSetTest.getTenantConnection;

const kUnknownCSPLogId = 6226300;
const kUnknownCSPLogComponent = 'control';
const kUnknownCSPLogLevel = 3;
const tenantId = ObjectId();

function runTest(conn) {
    const tenantConn = getTenantConnection(conn.host, tenantId);
    let i = 0;
    const connConfig = conn.getDB('config');
    for (let myConn of [conn, tenantConn]) {
        const myConnConfig = myConn.getDB('config');
        // Using non-tenant connection, check that there's no log message yet and set the log level
        // to debug
        assert(!checkLog.checkContainsOnceJson(conn, kUnknownCSPLogId, {name: 'foo_' + i}));
        const originalLogLevel =
            assert
                .commandWorked(connConfig.setLogLevel(kUnknownCSPLogLevel, kUnknownCSPLogComponent))
                .was.verbosity;

        // With given connection, insert into this tenant's cluster parameter collection
        assert.writeOK(myConnConfig.clusterParameters.insert(
            {_id: 'foo_' + i, clusterParameterTime: Date(), value: 123}));

        // With non-tenant connection, reset log level and check that the op observer triggered and
        // caused a log message about unknown cluster parameter
        assert.commandWorked(connConfig.setLogLevel(originalLogLevel, kUnknownCSPLogComponent));
        assert(checkLog.checkContainsOnceJson(conn, kUnknownCSPLogId, {name: 'foo_' + i}));
        i += 1;
    }
}

const rst = new ReplSetTest({nodes: 3});
rst.startSet({
    setParameter:
        {multitenancySupport: true, featureFlagMongoStore: true, featureFlagRequireTenantID: true}
});
rst.initiate();

// Create a root user within the multitenant environment so that getTenantConnection works.
assert.commandWorked(
    rst.getPrimary().getDB("admin").runCommand({createUser: "root", pwd: "pwd", roles: ["root"]}));

runTest(rst.getPrimary());
rst.stopSet();
})();
