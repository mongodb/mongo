/**
 * Tests for basic functionality of the rewriteCollection command.
 *
 * @tags: [
 *  requires_fcv_83,
 *  assumes_balancer_off,
 *  # Stepdown test coverage is already provided by the resharding FSM suites.
 *  does_not_support_stepdowns,
 *  # This test performs explicit calls to shardCollection
 *  assumes_unsharded_collection,
 * ]
 */

import {ReshardCollectionCmdTest} from "jstests/sharding/libs/reshard_collection_util.js";
import {getShardNames} from "jstests/sharding/libs/sharding_util.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

const shardNames = getShardNames(db);
const collName = jsTestName();
const dbName = db.getName();
const ns = dbName + "." + collName;
const mongos = db.getMongo();
const numInitialDocs = 500;
const zoneNames = ["z0", "z1", "z3"];

if (shardNames.length < 2) {
    jsTest.log.info(jsTestName() + " will not run; at least 2 shards are required.");
    quit();
}

const reshardCmdTest = new ReshardCollectionCmdTest({
    mongos,
    dbName,
    collName,
    numInitialDocs,
    skipDirectShardChecks: true,
});

jsTest.log.info("Succeed performing basic rewriteCollection command");
reshardCmdTest.assertReshardCollOk({rewriteCollection: ns, numInitialChunks: 2}, 2);

let additionalSetup = function (test) {
    const ns = test._ns;
    assert.commandWorked(mongos.adminCommand({addShardToZone: shardNames[0], zone: zoneNames[0]}));
    assert.commandWorked(mongos.adminCommand({addShardToZone: shardNames[1], zone: zoneNames[1]}));
};

jsTest.log.info("Succeed when rewriting all data to one zone/shard");
reshardCmdTest.assertReshardCollOk(
    {
        rewriteCollection: ns,
        numInitialChunks: 1,
        zones: [{zone: zoneNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}],
    },
    1,
    [{recipientShardId: shardNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}],
    [{zone: zoneNames[0], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}],
    additionalSetup,
);

jsTest.log.info("Fail when a zone with no shards is provided");
assert.throwsWithCode(
    () =>
        reshardCmdTest.assertReshardCollOk(
            {
                rewriteCollection: ns,
                zones: [{zone: zoneNames[2], min: {oldKey: MinKey}, max: {oldKey: MaxKey}}],
            },
            1,
        ),
    [ErrorCodes.BadValue, ErrorCodes.ZoneNotFound],
);

jsTest.log.info(
    "Succeed when reshardCollection changes the shard key after rewriteCollection is run but before it locks",
);

const rewritePauseBeforeLock = configureFailPoint(mongos, "hangRewriteCollectionBeforeRunningReshardCollection");

const awaitRewriteResult = startParallelShell(
    funWithArgs(function (ns) {
        assert.commandWorked(db.adminCommand({rewriteCollection: ns, numInitialChunks: 1}));
    }, ns),
    mongos.port,
);
rewritePauseBeforeLock.wait();

assert.commandWorked(mongos.adminCommand({reshardCollection: ns, key: {newKey: 1}, numInitialChunks: 1}));

// Verify that the shard key for the collection was changed to newKey
let collCount = mongos
    .getDB("config")
    .getCollection("collections")
    .find({_id: ns, key: {newKey: 1}})
    .itcount();
assert.eq(collCount, 1);

rewritePauseBeforeLock.off();
awaitRewriteResult();

// Verify that the shard key for the collection remained newKey after rewriteCollection completed
collCount = mongos
    .getDB("config")
    .getCollection("collections")
    .find({_id: ns, key: {newKey: 1}})
    .itcount();
assert.eq(collCount, 1);
