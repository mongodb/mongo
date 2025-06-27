/**
 * Test releaseMemory command for cursors with TEXT_OR stage.
 * @tags: [
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   does_not_support_transactions,
 *   not_allowed_with_signed_security_token,
 *   requires_fcv_82,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {
    accumulateServerStatusMetric,
    assertReleaseMemoryFailedWithCode,
    setAvailableDiskSpaceMode
} from "jstests/libs/release_memory_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

function getServerParameter(knob) {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [knob]: 1}))[knob];
}

function setServerParameter(knob, value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), knob, value);
}

db.dropDatabase();
const coll = db[jsTestName()];

function getTextOrSpillCounter() {
    return accumulateServerStatusMetric(db, metrics => metrics.query.textOr.spills);
}

const words1 = [
    "red",
    "orange",
    "yellow",
    "green",
    "blue",
    "purple",
];
const words2 = [
    "tea",
    "coffee",
    "soda",
    "water",
    "juice",
    "fresh",
];
const words3 = [
    "drink",
    "beverage",
    "refreshment",
    "hydration",
];

function insertData(coll, padding = "") {
    assert.commandWorked(coll.deleteMany({}));
    const docs = [];
    let price = 0;
    for (let word1 of words1) {
        for (let word2 of words2) {
            for (let word3 of words3) {
                docs.push(
                    {desc: word1 + " " + word2 + " " + word3, price: price, padding: padding});
                price = (price + 2) % 7;
            }
        }
    }
    assert.commandWorked(coll.insertMany(docs));
}

insertData(coll);
assert.commandWorked(coll.createIndex({desc: "text", price: 1}));

const predicate = {
    $text: {$search: "green tea drink"},
    price: {$lte: 5},
};

function idCompare(lhs, rhs) {
    return lhs._id.toString().localeCompare(rhs._id.toString());
}

let previousSpillCount = getTextOrSpillCounter();
const expectedResults =
    coll.find(predicate, {score: {$meta: "textScore"}}).toArray().sort(idCompare);
assert.eq(previousSpillCount, getTextOrSpillCounter());

{
    const cursor = coll.find(predicate, {score: {$meta: "textScore"}}).batchSize(1);
    const cursorId = cursor.getId();
    assert.eq(previousSpillCount, getTextOrSpillCounter());

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
    assert.lt(previousSpillCount, getTextOrSpillCounter());
    previousSpillCount = getTextOrSpillCounter();

    assert.eq(expectedResults, cursor.toArray().sort(idCompare));
}

{
    const cursor =
        coll.find(predicate, {score: {$meta: "textScore"}}).batchSize(1).allowDiskUse(false);
    const cursorId = cursor.getId();

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(
        releaseMemoryRes,
        cursorId,
        [ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed, ErrorCodes.ReleaseMemoryShardError]);

    assert.eq(previousSpillCount, getTextOrSpillCounter());

    assert.eq(expectedResults, cursor.toArray().sort(idCompare));
}

// No disk space available for spilling.
{
    jsTest.log(`Running releaseMemory with no disk space available`);
    const cursor =
        coll.find(predicate, {score: {$meta: "textScore"}}).batchSize(1).allowDiskUse(true);
    const cursorId = cursor.getId();

    // Release memory (i.e., spill)
    setAvailableDiskSpaceMode(db.getSiblingDB("admin"), 'alwaysOn');
    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);
    assertReleaseMemoryFailedWithCode(releaseMemoryRes, cursorId, ErrorCodes.OutOfDiskSpace);
    setAvailableDiskSpaceMode(db.getSiblingDB("admin"), 'off');

    jsTest.log.info("Running getMore");
    assert.throwsWithCode(() => cursor.toArray(), ErrorCodes.CursorNotFound);
}

{
    // Add a string of 2KB to each document
    insertData(coll, "x".repeat(2 * 1024));
    const expectedResults =
        coll.find(predicate, {padding: 0, score: {$meta: "textScore"}}).toArray().sort(idCompare);

    const originalKnobValue = getServerParameter("internalTextOrStageMaxMemoryBytes");
    setServerParameter("internalTextOrStageMaxMemoryBytes", 1 * 1024);
    const cursor = coll.find(predicate, {padding: 0, score: {$meta: "textScore"}}).batchSize(1);
    const cursorId = cursor.getId();
    // Because of extra data, TEXT_OR stage will spill automatically.
    assert.lt(previousSpillCount, getTextOrSpillCounter());
    previousSpillCount = getTextOrSpillCounter();

    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
    // Because TEXT_OR currently spills the last batch automatically, releaseMemory command
    // won't spill.
    assert.eq(previousSpillCount, getTextOrSpillCounter());

    assert.eq(expectedResults, cursor.toArray().sort(idCompare));
    setServerParameter("internalTextOrStageMaxMemoryBytes", originalKnobValue);
}
