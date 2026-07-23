/**
 * Verifies where query-memory load shedding runs in a sharded cluster: it is active on data-bearing
 * shard nodes (which publish RSS samples once enabled) but never on a pure router (mongos), whose RSS
 * monitor stays uninstalled and serverStatus().queryMemory stays absent. Routers are left out as a
 * first-version simplification (shedding is only safe where writes are exempt, and a router's writes
 * are all un-exempted remote dispatch to shards -- see README Limitations); every sharded node
 * carries the RouterServer role, so the predicate excludes only a router *exclusively* -- data-bearing
 * shards still run it.
 *
 * @tags: [
 *   requires_fcv_82,
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// serverStatus().queryMemory. Populated only while load shedding is supported on this process AND
// enabled AND running; an empty object otherwise (e.g. always on a pure router).
function memStats(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1})).queryMemory ?? {};
}

describe("query memory load shedding in a sharded cluster", function () {
    let st;
    let mongos;
    let shardPrimary;

    before(function () {
        // Enable the low mark on both the router and the shards. Small monitor interval so a shard's
        // first RSS sample lands quickly.
        const setParameter = {
            queryMemoryLoadSheddingLowMarkPercent: 50,
            queryMemoryRssMonitorIntervalMillis: 50,
        };
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
            other: {
                mongosOptions: {setParameter},
                rsOptions: {setParameter},
            },
        });
        mongos = st.s0;
        shardPrimary = st.rs0.getPrimary();
    });

    after(function () {
        st.stop();
    });

    it("runs on a data-bearing shard: it publishes an RSS sample", function () {
        // The shard mongod carries {ShardServer, RouterServer} but is data-bearing, so shedding is
        // supported there. Its monitor starts and soon publishes a real RSS sample.
        assert.soon(
            () => memStats(shardPrimary).loadShedding?.currentUsageBytes > 0,
            "a shard should start the monitor and publish an RSS sample",
        );
    });

    it("does not run on a pure router even when enabled", function () {
        // Enabled at startup, but a pure router never starts the monitor, so no metrics are exposed.
        assert.eq(
            undefined,
            memStats(mongos).loadShedding,
            "queryMemory load-shedding metrics should be absent on a pure router",
        );

        // Toggling the knob at runtime on the router is also a no-op: the on-update path does not
        // start the monitor there. Give the (nonexistent) monitor ample time to have published.
        assert.commandWorked(
            mongos.adminCommand({setParameter: 1, queryMemoryLoadSheddingLowMarkPercent: 60}),
        );
        sleep(500);
        assert.eq(
            undefined,
            memStats(mongos).loadShedding,
            "a pure router should never publish an RSS sample",
        );
    });
});
