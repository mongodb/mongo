/**
 * Test that ClusterServerParameterOpObserver fires appropriately on serverless replsets.
 * @tags: [
 *   does_not_support_stepdowns,
 *   requires_replication,
 *   requires_fcv_71,
 *   serverless
 *  ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    ChangeStreamMultitenantReplicaSetTest
} from "jstests/serverless/libs/change_collection_util.js";

const getTenantConnection = ChangeStreamMultitenantReplicaSetTest.getTenantConnection;

const tenantId = ObjectId();

function runTest(conn) {
    const tenantConn = getTenantConnection(conn.host, tenantId);
    let i = 0;
    for (let myConn of [conn, tenantConn]) {
        const myConnConfig = myConn.getDB('config');

        // With given connection, insert into this tenant's cluster parameter collection. Should
        // fail since this is an invalid parameter.
        const res = myConnConfig.clusterParameters.insert(
            {_id: 'foo_' + i, clusterParameterTime: Date(), value: 123});
        assert(res.hasWriteError());
        assert.neq(res.getWriteError().length, 0);
        i += 1;
    }
}

const rst = new ReplSetTest({nodes: 3});
rst.startSet({
    setParameter: {
        multitenancySupport: true,
        featureFlagRequireTenantID: true,
        featureFlagSecurityToken: true,
        testOnlyValidatedTenancyScopeKey: ChangeStreamMultitenantReplicaSetTest.getTokenKey(),
    }
});
rst.initiate();

// Create a root user within the multitenant environment so that getTenantConnection works.
assert.commandWorked(
    rst.getPrimary().getDB("admin").runCommand({createUser: "root", pwd: "pwd", roles: ["root"]}));

runTest(rst.getPrimary());
rst.stopSet();
