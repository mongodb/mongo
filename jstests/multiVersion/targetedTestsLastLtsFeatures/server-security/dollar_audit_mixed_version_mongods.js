/**
 * Tests $audit field metadata propagation across different FCVs in a sharded cluster
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

const oldVersion = "last-lts";
const newVersion = "latest";

function verifyProfilerEntries(testDB, comment, expectedSchema) {
    const profileEntry =
        testDB.system.profile.find({"command.comment": comment}).hint({$natural: 1}).toArray()[0];

    assert.neq(profileEntry, null, `No matching profile entry found on shard`);
    assert.docEq(
        expectedSchema, profileEntry.command.$audit, `Expected metadata doesn't match on shard`);
}

function forceMetadataPropagation(testDB, comment) {
    assert.commandWorked(testDB.user.insert({_id: 1, value: "test"}, {comment: comment}));
    assert.commandWorked(testDB.runCommand({
        find: "user",
        comment: comment,
    }));
}

function testDollarAuditPropagation(st, fcv, isLoadBalanced = false, proxy_server = null) {
    const profilerComment = `fcv_${fcv}_$audit_test`;
    let ingressPort;
    let egressPort;

    const mongos = st.s;
    const shard = st.shard0.getDB("test");
    const adminDB = mongos.getDB("admin");
    let testDB = mongos.getDB("test");
    let shellPort = mongos.getShellPort();
    let mongosPort = mongos.port;

    if (isLoadBalanced) {
        ingressPort = proxy_server.getIngressPort();
        egressPort = proxy_server.getEgressPort();

        const uri = `mongodb://127.0.0.1:${ingressPort}/?loadBalanced=true`;
        const conn = new Mongo(uri);

        testDB = conn.getDB("test");
        shellPort = conn.getShellPort();
        mongosPort = egressPort;
    }

    let dollarAuditMetadata = {
        $impersonatedUser: {user: "testUser", db: "test"},
        $impersonatedRoles: [{role: "readWrite", db: "test"}]
    };
    if (fcv === latestFCV) {
        dollarAuditMetadata.$impersonatedClient = {
            hosts: [`127.0.0.1:${shellPort}`, `127.0.0.1:${mongosPort}`]
        };
        if (isLoadBalanced) {
            dollarAuditMetadata.$impersonatedClient.hosts.push(`127.0.0.1:${ingressPort}`);
        }
    }

    assert.commandWorked(shard.setProfilingLevel(2));

    assert.commandWorked(
        testDB.runCommand({createUser: 'testUser', pwd: 'testUser', roles: ['readWrite']}));
    assert.eq(1, testDB.auth("testUser", "testUser"));

    assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: fcv, confirm: true}));
    checkFCV(adminDB, fcv);

    // TODO SERVER-83990: remove.
    // During an upgrade, the FCV is first changed on the the shard and then on the config server.
    // Thus, since we are checking the $audit field metadata propagation across different FCVs in a
    // sharded cluster, we need to check the feature flag directly on the shard.
    const shardAdminDB = st.shard0.getDB("admin");
    if (fcv === latestFCV) {
        assert(FeatureFlagUtil.isPresentAndEnabled(shardAdminDB, "ExposeClientIpInAuditLogs"));
    } else {
        assert(!FeatureFlagUtil.isPresentAndEnabled(shardAdminDB, "ExposeClientIpInAuditLogs"));
    }

    forceMetadataPropagation(testDB, profilerComment);
    verifyProfilerEntries(testDB, profilerComment, dollarAuditMetadata);
}

{
    // Test the expected metadata is propagated in a cluster using the latestFCV.
    jsTest.log(`Testing FCV ${latestFCV} metadata propagation in sharded cluster`);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            },
            configOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            },
            shardOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            }
        }
    });

    testDollarAuditPropagation(st, latestFCV);
    st.stop();
}

{
    // Test the expected metadata is propagated in a cluster using the lastLTS fcv.
    jsTest.log(
        `Testing FCV ${lastLTSFCV} metadata propagation in sharded cluster with mixed versions`);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {binVersion: oldVersion, auditDestination: 'console'},
            configOptions: {binVersion: oldVersion, auditDestination: 'console'},
            shardOptions: {binVersion: oldVersion, auditDestination: 'console'}
        }
    });

    testDollarAuditPropagation(st, lastLTSFCV);
    st.stop();
}

{
    // Test the expected metadata is propagated in a cluster using the lastLTS fcv.
    jsTest.log(
        `Testing FCV ${lastLTSFCV} metadata propagation in sharded cluster with mixed versions`);
    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {binVersion: oldVersion, auditDestination: 'console'},
            configOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            },
            shardOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            }
        }
    });

    testDollarAuditPropagation(st, lastLTSFCV);
    st.stop();
}

{
    // Test the expected metadata is propagated in a load balanced cluster using the latestFCV.
    jsTest.log(
        `Testing FCV ${latestFCV} metadata propagation in sharded cluster with mixed versions with a load balanced connection`);

    const ingressPort = allocatePort();
    const egressPort = allocatePort();

    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1);
    proxy_server.start();

    const st = new ShardingTest({
        shards: 1,
        mongos: 1,
        config: 1,
        other: {
            mongosOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
                setParameter: {
                    "loadBalancerPort": egressPort,
                },
            },
            configOptions: {
                binVersion: newVersion,
                auditDestination: 'console',
            },
            shardOptions: {
                binVersion: newVersion,
            }
        }
    });

    testDollarAuditPropagation(st, latestFCV, true /* isLoadBalanced */, proxy_server);
    st.stop();
    proxy_server.stop();
}
