import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {isLinux} from "jstests/libs/os_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";

if (!isLinux()) {
    jsTestLog("Test is only available on Linux platforms.");
    quit();
}

const ingressAddress = "127.0.0.1";
const egress_address = get_ipaddr();
const ingressPort = allocatePort();
const egressPort = allocatePort();
const proxy_server =
    new ProxyProtocolServer(ingressPort, egressPort, 1, ingressAddress, egress_address);
proxy_server.start();
const opts = {
    mongos: 1,
    config: 1,
    shards: 1,
    mongosOptions: {setParameter: {loadBalancerPort: egressPort}},
};
const fixture = new ShardingTest(opts);
const proxyUri = `mongodb://${ingressAddress}:${ingressPort}/?loadBalanced=true`;
const proxyMongo = new Mongo(proxyUri);
const proxyDB = proxyMongo.getDB("admin");
const appendResult = assert.commandWorked(proxyDB.runCommand({whatsmyuri: 1}));
assert.eq(
    appendResult.sourceClientIP, ingressAddress, "sourceClientIP should match ingress address");

fixture.stop();
proxy_server.stop();
