/**
 * Test that the net.secondaryPort configuration works in a sharded cluster.
 *
 * Topology:
 *   - 2 mongos: s0 has a secondary port, s1 does not.
 *   - 1 single-node config server replica set with a secondary port.
 *   - 2 single-node shard replica sets, each with a secondary port.
 *
 * Validates:
 *   - Reads and writes on both mongos via main ports.
 *   - Reads and writes on the mongos secondary port.
 *   - hello on all secondary ports (config server and both shards).
 *
 * @tags: [
 *   requires_sharding,
 *   # The secondary port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

const mongosSecondaryPort = allocatePort();
const configSecondaryPort = allocatePort();
const shard0SecondaryPort = allocatePort();
const shard1SecondaryPort = allocatePort();
const dbName = jsTestName();
const collName = "testcoll";

describe("secondary port in a sharded cluster", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: 2,
            mongos: 2,
            config: 1,
            s0: {secondaryPort: mongosSecondaryPort},
            configOptions: {secondaryPort: configSecondaryPort},
            rs0: {nodes: 1, secondaryPort: shard0SecondaryPort},
            rs1: {nodes: 1, secondaryPort: shard1SecondaryPort},
        });
    });

    after(function () {
        this.st.stop();
    });

    it("should support reads and writes on both mongos via main ports", function () {
        for (const mongos of [this.st.s0, this.st.s1]) {
            const coll = mongos.getDB(dbName).getCollection(collName);
            const id = "main_" + mongos.port;

            assert.commandWorked(coll.insert({_id: id, src: "mainPort"}));
            const doc = coll.findOne({_id: id});
            assert.neq(doc, null, "should read back document via main port on mongos " + mongos.host);
            assert.eq(doc.src, "mainPort");
        }
    });

    it("should support reads and writes on the mongos secondary port", function () {
        // TODO SERVER-121779 hostNoPort is not set on MongoRunner.runMongos
        const host = this.st.s0.host.split(":")[0];
        const conn = new Mongo(`${host}:${mongosSecondaryPort}`);
        const coll = conn.getDB(dbName).getCollection(collName);

        assert.commandWorked(coll.insert({_id: "secondary_mongos", src: "secondaryPort"}));
        const doc = coll.findOne({_id: "secondary_mongos"});
        assert.neq(doc, null, "should read back document via mongos secondary port");
        assert.eq(doc.src, "secondaryPort");

        const mainDoc = coll.findOne({_id: "main_" + this.st.s0.port});
        assert.neq(mainDoc, null, "document from main port should be readable via mongos secondary port");

        conn.close();
    });

    it("should respond to hello on the config server secondary port", function () {
        const configNode = this.st.configRS.nodes[0];
        const conn = new Mongo(`${configNode.hostNoPort}:${configSecondaryPort}`);

        const helloRes = assert.commandWorked(conn.adminCommand({hello: 1}));
        assert.eq(helloRes.ok, 1, "hello on config server secondary port should succeed");
        assert(helloRes.isWritablePrimary, "config server should report isWritablePrimary on secondary port");
        assert(helloRes.hasOwnProperty("setName"), "config server hello should include setName on secondary port");

        conn.close();
    });

    it("should respond to hello on shard 0 secondary port", function () {
        const shardNode = this.st.rs0.nodes[0];
        const conn = new Mongo(`${shardNode.hostNoPort}:${shard0SecondaryPort}`);

        const helloRes = assert.commandWorked(conn.adminCommand({hello: 1}));
        assert.eq(helloRes.ok, 1, "hello on shard0 secondary port should succeed");
        assert(helloRes.isWritablePrimary, "shard0 should report isWritablePrimary on secondary port");
        assert(helloRes.hasOwnProperty("setName"), "shard0 hello should include setName on secondary port");

        conn.close();
    });

    it("should respond to hello on shard 1 secondary port", function () {
        const shardNode = this.st.rs1.nodes[0];
        const conn = new Mongo(`${shardNode.hostNoPort}:${shard1SecondaryPort}`);

        const helloRes = assert.commandWorked(conn.adminCommand({hello: 1}));
        assert.eq(helloRes.ok, 1, "hello on shard1 secondary port should succeed");
        assert(helloRes.isWritablePrimary, "shard1 should report isWritablePrimary on secondary port");
        assert(helloRes.hasOwnProperty("setName"), "shard1 hello should include setName on secondary port");

        conn.close();
    });
});
