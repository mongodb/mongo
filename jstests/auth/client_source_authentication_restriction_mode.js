// Test that authentication restrictions can be applied on origin clients or direct peers using
// the clientSourceAuthenticationRestrictionMode server parameter.

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {
    isLinux,
} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    ProxyProtocolServer,
} from "jstests/sharding/libs/proxy_protocol.js";

// TODO: SERVER-100859: remove
// Proxy protocol server does not work on non-Linux platforms.
if (!isLinux()) {
    jsTestLog("Test is only available on linux platforms.");
    quit();
}

function runTest(mongosCon, mode, proxyIngressAddress, proxyIngressPort, restrictionAddress) {
    // Use a direct, non-proxied connection to the server to run setup operations.
    const adminMongo = new Mongo(mongosCon.host);
    const adminDB = adminMongo.getDB("admin");

    // Setup a user that can only authenticate from restrictionAddress. The origin client's IP
    // address is expected to satisfy restrictionAddress while the proxy protocol server's egress
    // address does not.
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert(adminDB.auth("admin", "admin"));
    assert.commandWorked(adminDB.runCommand({
        createUser: "testUser",
        pwd: "testUser",
        roles: ["root"],
        authenticationRestrictions: [{clientSource: [restrictionAddress]}]
    }));

    // Authenticate as testUser via a separate connection that passes through the proxy server.
    const proxyServerUri =
        `mongodb://${proxyIngressAddress}:${proxyIngressPort}/?loadBalanced=true`;
    const proxiedMongo = new Mongo(proxyServerUri);
    const proxiedAdminDB = proxiedMongo.getDB("admin");

    assert(mode === 'origin' || mode === 'peer');
    if (mode === "origin") {
        assert(proxiedAdminDB.auth('testUser', 'testUser'));
    } else {
        assert(!proxiedAdminDB.auth('testUser', 'testUser'));
    }
}

// Interface for mongo shell <-> proxy protocol server ingress is 127.0.0.1
// Interface for proxy protocol server egress <-> mongos is public IP address of the host.
const ingressAddress = '127.0.0.1';
const egressAddress = get_ipaddr();
const restrictionAddress = ingressAddress;
const ingressPort = allocatePort();
const egressPort = allocatePort();
const proxyServer = new ProxyProtocolServer(ingressPort, egressPort, 1);
proxyServer.ingress_address = ingressAddress;
proxyServer.egress_address = egressAddress;
proxyServer.start();

{
    const opts = {
        mongos: 1,
        config: 1,
        shards: 1,
        mongosOptions: {
            setParameter:
                {loadBalancerPort: egressPort, clientSourceAuthenticationRestrictionMode: 'origin'}
        },
    };
    const fixture = new ShardingTest(opts);
    runTest(fixture.s0, 'origin', ingressAddress, ingressPort, restrictionAddress);
    fixture.stop();
}

{
    const opts = {
        mongos: 1,
        config: 1,
        shards: 1,
        mongosOptions: {
            setParameter:
                {loadBalancerPort: egressPort, clientSourceAuthenticationRestrictionMode: 'peer'}
        },
    };
    const fixture = new ShardingTest(opts);
    runTest(fixture.s0, 'peer', ingressAddress, ingressPort, restrictionAddress);
    fixture.stop();
}

proxyServer.stop();
