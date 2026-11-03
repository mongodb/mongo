/**
 * Verifies concurrent resharding with featureFlagConcurrentResharding enabled. Covers three
 * scenarios: two concurrent reshardCollection operations, two concurrent moveCollection
 * operations, and one reshardCollection concurrent with one moveCollection. Each scenario checks
 * that both coordinators are active at the same time, lets them run to completion, and then
 * verifies the resulting shard key or shard placement.
 *
 * @tags: [
 *   featureFlagConcurrentResharding,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runReshardCollection(mongosHost, ns, newShardKeyField) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({
        reshardCollection: ns,
        key: {[newShardKeyField]: 1},
        numInitialChunks: 1,
    });
}

function runMoveCollection(mongosHost, ns, toShard) {
    const mongos = new Mongo(mongosHost);
    return mongos.adminCommand({moveCollection: ns, toShard: toShard});
}

function runBothWithCoordinatorsActive(st, nss, thread1, thread2) {
    const pauseBeforeCloningFp = configureFailPoint(
        st.configRS.getPrimary(),
        "reshardingPauseCoordinatorBeforeCloning",
    );

    try {
        thread1.start();
        thread2.start();

        pauseBeforeCloningFp.wait({timesEntered: 2});

        const activeOps = st.s
            .getDB("config")
            .reshardingOperations.find({ns: {$in: nss}})
            .toArray();
        assert.eq(2, activeOps.length, "Expected both coordinators to be active", {activeOps});
    } finally {
        pauseBeforeCloningFp.off();
    }

    thread1.join();
    thread2.join();

    assert.commandWorked(thread1.returnData());
    assert.commandWorked(thread2.returnData());
}

describe("concurrent resharding operations on different namespaces in the same database", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2});

        this.dbName = "test_concurrent_resharding";
        this.fromShard = this.st.shard0.shardName;
        this.toShard = this.st.shard1.shardName;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.fromShard}),
        );

        this.makeShardedColl = (collName, newKeyField) => {
            const ns = `${this.dbName}.${collName}`;
            assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {oldKey: 1}}));
            const coll = this.st.s.getCollection(ns);
            for (let i = 0; i < 100; i++) {
                assert.commandWorked(coll.insert({oldKey: i, [newKeyField]: -i}));
            }
            return ns;
        };

        this.makeUnsplittableColl = (collName) => {
            const ns = `${this.dbName}.${collName}`;
            assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({create: collName}));
            const coll = this.st.s.getCollection(ns);
            for (let i = 0; i < 100; i++) {
                assert.commandWorked(coll.insert({_id: i}));
            }
            return ns;
        };

        this.assertMoved = (ns) => {
            assert.eq(100, this.st.rs1.getPrimary().getCollection(ns).countDocuments({}), {ns});
            assert.eq(0, this.st.rs0.getPrimary().getCollection(ns).countDocuments({}), {ns});
        };
    });

    after(() => {
        this.st.stop();
    });

    it("allows two reshardCollection operations to run concurrently", () => {
        const ns1 = this.makeShardedColl("reshardColl1", "newKey1");
        const ns2 = this.makeShardedColl("reshardColl2", "newKey2");

        runBothWithCoordinatorsActive(
            this.st,
            [ns1, ns2],
            new Thread(runReshardCollection, this.st.s.host, ns1, "newKey1"),
            new Thread(runReshardCollection, this.st.s.host, ns2, "newKey2"),
        );

        const configColl = this.st.s.getDB("config").collections;
        assert.docEq({newKey1: 1}, configColl.findOne({_id: ns1}).key);
        assert.docEq({newKey2: 1}, configColl.findOne({_id: ns2}).key);
    });

    it("allows two moveCollection operations to run concurrently", () => {
        const ns1 = this.makeUnsplittableColl("moveColl1");
        const ns2 = this.makeUnsplittableColl("moveColl2");

        runBothWithCoordinatorsActive(
            this.st,
            [ns1, ns2],
            new Thread(runMoveCollection, this.st.s.host, ns1, this.toShard),
            new Thread(runMoveCollection, this.st.s.host, ns2, this.toShard),
        );

        this.assertMoved(ns1);
        this.assertMoved(ns2);
    });

    it("allows a reshardCollection and a moveCollection to run concurrently", () => {
        const reshardNs = this.makeShardedColl("mixedReshardColl", "newKey");
        const moveNs = this.makeUnsplittableColl("mixedMoveColl");

        runBothWithCoordinatorsActive(
            this.st,
            [reshardNs, moveNs],
            new Thread(runReshardCollection, this.st.s.host, reshardNs, "newKey"),
            new Thread(runMoveCollection, this.st.s.host, moveNs, this.toShard),
        );

        const configColl = this.st.s.getDB("config").collections;
        assert.docEq({newKey: 1}, configColl.findOne({_id: reshardNs}).key);
        this.assertMoved(moveNs);
    });
});
