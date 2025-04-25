import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {
    isLinux,
} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    ProxyProtocolServer,
} from "jstests/sharding/libs/proxy_protocol.js";

function runTest(
    mongosCon, clientSourceAuthenticationRestrictionMode, ingressAddress, ingressPort) {
    const uriExt = `mongodb://${ingressAddress}:${ingressPort}/?loadBalanced=true`;

    // Create Admin Client using the MongoDB URI
    const adminMongo = new Mongo(mongosCon.host);
    const adminDB = adminMongo.getDB("admin");

    // Create Admin User
    assert.commandWorked(adminDB.runCommand({createUser: "admin", pwd: "admin", roles: ["root"]}));
    assert(adminDB.auth("admin", "admin"));

    // External Client with Auth Restrictions
    const externalMongo = new Mongo(uriExt);
    const externalDb = externalMongo.getDB("admin");

    // Test User with Auth Restrictions
    assert.commandWorked(adminDB.runCommand({
        createUser: "testUser",
        pwd: "testUser",
        roles: ["root"],
        authenticationRestrictions: [{clientSource: [ingressAddress]}]
    }));

    // if else assertions
    assert(clientSourceAuthenticationRestrictionMode === 'origin' ||
           clientSourceAuthenticationRestrictionMode === 'peer');
    if (clientSourceAuthenticationRestrictionMode === "origin") {
        assert(externalDb.auth('testUser', 'testUser'));
    } else {
        assert(!externalDb.auth('testUser', 'testUser'));
    }
}

{
    if (!isLinux()) {
        jsTestLog("Test is only available on linux platforms.");
        quit();
    }
    const restrictionMode = 'origin';
    const ingressAddress = '127.0.0.2';
    const ingressPort = allocatePort();
    const egressPort = allocatePort();
    const proxy_server = new ProxyProtocolServer(ingressPort, egressPort, 1, ingressAddress);
    proxy_server.start();
    const opts = {
        mongos: 1,
        config: 1,
        shards: 1,
        mongosOptions: {
            setParameter: {
                loadBalancerPort: egressPort,
                clientSourceAuthenticationRestrictionMode: restrictionMode
            }
        },
    };
    const fixture = new ShardingTest(opts);
    runTest(fixture.s0, restrictionMode, ingressAddress, ingressPort);
    fixture.stop();
    // TODO SERVER-104272 add testing for the 'peer' case
    proxy_server.stop();
}
