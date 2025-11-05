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
