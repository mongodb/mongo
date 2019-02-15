/**
 * This test does a full rollover of the X509 auth for cluster membership. After the rollover,
 * the cluster will have a new CA and the dn components used to determine cluster membership
 * will have changed;
 *
 * @tags: [requires_persistence, requires_replication]
 */

(function() {
    'use strict';

    const rst = new ReplSetTest({
        nodes: 3,
        waitForKeys: false,
        nodeOptions: {
            sslMode: "preferSSL",
            clusterAuthMode: "x509",
            sslPEMKeyFile: "jstests/libs/server.pem",
            sslCAFile: "jstests/libs/ca.pem",
            sslAllowInvalidHostnames: ""
        }
    });
    rst.startSet();

    rst.initiateWithAnyNodeAsPrimary(
        Object.extend(rst.getReplSetConfig(), {writeConcernMajorityJournalDefault: true}));

    // Create a user to login as when auth is enabled later
    rst.getPrimary().getDB('admin').createUser({user: 'root', pwd: 'root', roles: ['root']},
                                               {w: 3});
    rst.nodes.forEach((node) => {
        assert(node.getDB("admin").auth("root", "root"));
    });

    // All the certificates' DNs share this base
    const dnBase = "C=US, ST=New York, L=New York,";
    // This is the DN of the rollover certificate.
    const rolloverDN = dnBase + " O=MongoDB\\, Inc. (Rollover), OU=Kernel (Rollover), CN=server";
    // This is the DN of the original certificate
    const originalDN = dnBase + " O=MongoDB, OU=Kernel, CN=server";

    // This will rollover the cluster to a new config in a rolling fashion. It will return when
    // there is a primary and we are able to write to it.
    const rolloverConfig = function(newConfig) {
        const restart = function(node) {
            const nodeId = rst.getNodeId(node);
            rst.stop(nodeId);
            const configId = "n" + nodeId;
            rst.nodeOptions[configId] = Object.merge(rst.nodeOptions[configId], newConfig, true);
            const newNode = rst.start(nodeId, {}, true, true);
            assert(newNode.getDB("admin").auth("root", "root"));
        };

        rst.getSecondaries().forEach(function(secondary) {
            restart(secondary);
        });

        restart(rst.getPrimary());

        assert.soon(() => {
            let primary = rst.getPrimary();
            assert.commandWorked(primary.getDB("admin").runCommand({isMaster: 1}));
            assert.writeOK(primary.getDB('test').a.insert({a: 1, str: 'TESTTESTTEST'}));

            // Start a shell that connects to the server with the current CA/cert configuration
            // and ensure that it's able to connect and authenticate with x509.
            const shellArgs = [
                'mongo',
                primary.name,
                '--eval',
                ';',
                '--ssl',
                '--sslAllowInvalidHostnames',
                '--sslCAFile',
                newConfig['sslCAFile'],
                '--sslPEMKeyFile',
                newConfig['sslPEMKeyFile'],
                '--authenticationDatabase=$external',
                '--authenticationMechanism=MONGODB-X509'
            ];
            assert.eq(_runMongoProgram.apply(null, shellArgs), 0);

            return true;
        });
    };

    jsTestLog("Rolling over CA certificate to combined old and new CA's");
    rolloverConfig({
        sslPEMKeyFile: "jstests/libs/server.pem",
        sslCAFile: "jstests/libs/rollover_ca_merged.pem",
        setParameter: {
            tlsX509ClusterAuthDNOverride: rolloverDN,
        }
    });

    jsTestLog("Rolling over to new certificate with new cluster DN and new CA");
    rolloverConfig({
        sslPEMKeyFile: "jstests/libs/rollover_server.pem",
        sslCAFile: "jstests/libs/rollover_ca_merged.pem",
        setParameter: {
            tlsX509ClusterAuthDNOverride: originalDN,
        }
    });

    jsTestLog("Rolling over to new CA only");
    rolloverConfig({
        sslPEMKeyFile: "jstests/libs/rollover_server.pem",
        sslCAFile: "jstests/libs/rollover_ca.pem",
    });

    rst.stopSet();
})();
