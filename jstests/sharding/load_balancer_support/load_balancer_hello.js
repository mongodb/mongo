/**
 * Test the extension to the mongos `hello` command by which clients
 * that have arrived through a load balancer affirm that they are
 * compatible with the way mongos handles load-balanced clients.
 * See `src/mongo/s/load_balancing_support.h`.
 *
 * @tags: [
 *    requires_fcv_81,
 * ]
 *
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

(() => {
    /**
     * The whole ShardingTest is restarted just to get a fresh connection.
     * Obviously this could be accomplished much more efficiently.
     */
    let runInShardingTest = (func) => {
        let st = new ShardingTest({shards: 1, mongos: 1});
        try {
            func(st.s0.getDB("admin"));
        } finally {
            st.stop();
        }
    };

    let doHello = (admin, {lbConnection, lbHello}) => {
        if (lbConnection)
            assert.commandWorked(
                admin.adminCommand({configureFailPoint: "clientIsConnectedToLoadBalancerPort", mode: "alwaysOn"}),
            );
        try {
            let helloDoc = {};
            if (lbHello) helloDoc["loadBalanced"] = true;
            return admin.runCommand("hello", helloDoc);
        } finally {
            assert.commandWorked(
                admin.adminCommand({configureFailPoint: "clientIsConnectedToLoadBalancerPort", mode: "off"}),
            );
        }
    };

    let assertServiceId = (res) => {
        assert.commandWorked(res);
        assert(res.hasOwnProperty("serviceId"), "serviceId missing from hello response:" + tojson(res));
        assert(res.serviceId.isObjectId, "res.serviceId = " + tojson(res.serviceId));
    };

    let assertNoServiceId = (res) => {
        assert.commandWorked(res);
        assert(!res.hasOwnProperty("serviceId"), "res.serviceId = " + tojson(res.serviceId));
    };

    /*
     * The ordinary baseline non-load-balanced case.
     */
    runInShardingTest((admin) => {
        jsTestLog("Initial hello command");
        assertNoServiceId(doHello(admin, {}));
        jsTestLog("Non-initial hello command");
        assertNoServiceId(doHello(admin, {}));
    });

    /*
     * Good case: client arrived via load balancer, and load balancing declared by client.
     * The load balancing `serviceId` reporting applies only to the initial hello.
     * The `loadBalanced` option is ignored in subsequent `hello` commands.
     */
    runInShardingTest((admin) => {
        assertServiceId(doHello(admin, {lbConnection: true, lbHello: true}));
        assertNoServiceId(doHello(admin, {lbConnection: true, lbHello: true}));
    });

    /*
     * Client did not arrive via load-balancer, but load balancing support declared by client.
     * We tolerate the `isLoadBalanced` option but ignore it.
     */
    runInShardingTest((admin) => {
        assertNoServiceId(doHello(admin, {lbHello: true}));
    });

    /*
     * Client arrived via load balancer, but load balancing support was not declared by client.
     * This is an error that should result in a disconnection.
     */
    runInShardingTest((admin) => {
        assertNoServiceId(doHello(admin, {lbConnection: true}));
    });
})();
