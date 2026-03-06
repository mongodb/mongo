/**
 * Test transition to dedicated config server waits for orphanCleanupDelaySecs before dropping
 * local collections.
 * @tags: [
 * requires_fcv_82,
 * # This test disables the range deleter (disableResumableRangeDeleter: true) to test orphan
 * # cleanup delay. Stepdowns during moveChunk create range deletion tasks that can't be cleaned
 * # up, causing subsequent moveChunk retries to fail with "overlapping range deletion" errors.
 * does_not_support_stepdowns
 * ]
 */

import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function insertDocs(coll, numDocs) {
    const docs = [];
    for (let i = 0; i < numDocs; i++) {
        docs.push({_id: i, data: "x".repeat(10)});
    }
    assert.commandWorked(coll.insertMany(docs));
}

describe("Test transition to dedicated config server waits for orphanCleanupDelaySecs", function() {
    const orphanCleanupDelaySecs = 3;

    before(() => {
        jsTest.log.info(
            "Create sharded cluster 'stOrphanDelay' with embedded config server and orphanCleanupDelaySecs > 0",
        );
        this.st = new ShardingTest({
            name: jsTestName() + "_orphanDelay",
            shards: 2,
            rs: {nodes: 3},
            other: {
                enableBalancer: true,
                configShard: true,
                rsOptions: {
                    setParameter: {
                        orphanCleanupDelaySecs: orphanCleanupDelaySecs,
                        // Disable range deleter to ensure range deletion tasks remain in DB
                        // for the transition code to find and wait for orphanCleanupDelaySecs
                        disableResumableRangeDeleter: true,
                    },
                },
            },
        });
    });

    after(() => {
        jsTest.log.info("Stop sharded cluster 'stOrphanDelay'");
        this.st.stop();
    });

    afterEach(() => {
        const numShards = this.st.s.getDB("config").shards.count();
        if (numShards == 2) {
            jsTest.log.info(
                "Cancel transition to dedicated config server by unsetting draining flag");
            this.st.configRS.getPrimary().getDB("config").shards.updateOne(
                {_id: "config", draining: true}, {$unset: {draining: ""}});
            const drainingShards =
                this.st.s.getDB("config").shards.find({"draining": true}).toArray();
            assert.eq(0, drainingShards.length);
        } else {
            jsTest.log.info("Transition back to embedded config server");
            assert.commandWorked(this.st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));
        }
    });

    it("Test transition completes with delayed range deletion tasks after waiting", function() {
        const dbName = jsTestName() + "_delayed";
        const collName = "testColl";
        const ns = dbName + "." + collName;

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}));
        assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

        const coll = this.st.s.getDB(dbName).getCollection(collName);
        insertDocs(coll, 100);

        // Split and migrate a chunk to create range deletion tasks
        assert.soon(
            () => this.st.s.adminCommand({split: ns, middle: {_id: 50}}).ok,
            "split did not complete within the timeout",
        );
        assert.soon(
            () => this.st.s
                      .adminCommand({
                          moveChunk: ns,
                          find: {_id: 0},
                          to: this.st.shard1.shardName,
                          _waitForDelete: false,
                      })
                      .ok,
            "moveChunk did not complete within the timeout",
        );

        // Move the other chunk as well
        assert.soon(
            () => this.st.s
                      .adminCommand({
                          moveChunk: ns,
                          find: {_id: 50},
                          to: this.st.shard1.shardName,
                          _waitForDelete: false,
                      })
                      .ok,
            "moveChunk did not complete within the timeout",
        );

        // Move primary to non-config shard
        assert.soon(
            () => this.st.s.adminCommand({movePrimary: dbName, to: this.st.shard1.shardName}).ok,
            "movePrimary did not complete within the timeout",
        );

        jsTest.log.info("Start transition to dedicated config server");
        let res = this.st.s.adminCommand({transitionToDedicatedConfigServer: 1});
        assert.commandWorked(res);
        assert.eq("started", res.state, "Expected 'started' state: " + tojson(res));
        this.st.configRS.awaitReplication();

        jsTest.log.info(
            "Wait for transition to complete (verifying orphan cleanup delay is enforced)");
        let sawPendingDataCleanup = false;
        assert.soon(
            () => {
                res = this.st.s.adminCommand({transitionToDedicatedConfigServer: 1});
                assert.commandWorked(res);
                if (res.state === "pendingDataCleanup") {
                    sawPendingDataCleanup = true;
                }
                return res.state === "completed";
            },
            "transitionToDedicatedConfigServer did not reach 'completed' state within the timeout",
            120000,
        );

        assert(
            sawPendingDataCleanup,
            "Expected to observe 'pendingDataCleanup' state while waiting for orphanCleanupDelaySecs");

        // Verify config shard is no longer in config.shards
        const configShard = this.st.s.getDB("config").shards.findOne({_id: "config"});
        assert.eq(null, configShard, "Config shard should have been removed");

        assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());
    });

    it("Test queries started before draining fail with QueryPlanKilled after collection drop",
       function() {
           const dbName = jsTestName() + "_qryKilled";
           const collName = "testColl";
           const ns = dbName + "." + collName;

           assert.commandWorked(
               this.st.s.adminCommand({enableSharding: dbName, primaryShard: "config"}));
           assert.commandWorked(this.st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

           // Insert enough data to require multiple batches (default batch size is 101)
           const coll = this.st.s.getDB(dbName).getCollection(collName);
           insertDocs(coll, 100);

           this.st.configRS.awaitReplication();

           // Open a cursor via mongos with secondary read preference
           // Secondary reads will fail with QueryPlanKilled when the collection is dropped.
           jsTest.log.info(
               "Opening cursor via mongos with secondary read preference before draining starts");
           const cursor = this.st.s.getDB(dbName)
                              .getCollection(collName)
                              .find({})
                              .readPref("secondary")
                              .batchSize(10);

           // Fetch the first batch to establish the cursor
           const firstDoc = cursor.next();
           assert.neq(null, firstDoc, "Should have documents in the collection");
           const cursorId = cursor.getId();
           assert.neq(0, cursorId, "Cursor should be open with more documents to fetch");

           // Move all chunks off the config shard
           assert.soon(
               () => this.st.s.adminCommand({split: ns, middle: {_id: 250}}).ok,
               "split did not complete within the timeout",
           );
           assert.soon(
               () => this.st.s
                         .adminCommand({
                             moveChunk: ns,
                             find: {_id: 0},
                             to: this.st.shard1.shardName,
                             _waitForDelete: false,
                         })
                         .ok,
               "moveChunk did not complete within the timeout",
           );
           assert.soon(
               () => this.st.s
                         .adminCommand({
                             moveChunk: ns,
                             find: {_id: 250},
                             to: this.st.shard1.shardName,
                             _waitForDelete: false,
                         })
                         .ok,
               "moveChunk did not complete within the timeout",
           );

           // Move primary to non-config shard
           assert.soon(
               () => this.st.s.adminCommand({movePrimary: dbName, to: this.st.shard1.shardName}).ok,
               "movePrimary did not complete within the timeout",
           );

           jsTest.log.info("Start transition to dedicated config server");
           let res = this.st.s.adminCommand({transitionToDedicatedConfigServer: 1});
           assert.commandWorked(res);
           assert.eq("started", res.state, "Expected 'started' state: " + tojson(res));
           this.st.configRS.awaitReplication();

           jsTest.log.info("Wait for transition to complete");
           assert.soon(
               () => {
                   res = this.st.s.adminCommand({transitionToDedicatedConfigServer: 1});
                   assert.commandWorked(res);
                   return res.state === "completed";
               },
               "transitionToDedicatedConfigServer did not reach 'completed' state within the timeout",
               120000,
           );

           const error = assert.throws(() => {
               while (cursor.hasNext()) {
                   cursor.next();
               }
           });

           assert.eq(ErrorCodes.QueryPlanKilled,
                     error.code,
                     "Expected QueryPlanKilled error but got: " + tojson(error));

           jsTest.log.info("Transition back to embedded config server");
           assert.commandWorked(this.st.s.adminCommand({transitionFromDedicatedConfigServer: 1}));

           assert.commandWorked(this.st.s.getDB(dbName).dropDatabase());
       });
});
