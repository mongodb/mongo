/**
 * Test how releaseMemory command interacts with snapshot read concern and collection drops
 * @tags: [
 *   # TODO SERVER-97456 - this test should work with mongos when command is supported
 *   assumes_against_mongod_not_mongos,
 *   assumes_read_preference_unchanged,
 *   assumes_superuser_permissions,
 *   requires_fcv_81,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 *   uses_snapshot_read_concern,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
const rstPrimary = rst.getPrimary();
const db = rstPrimary.getDB(jsTestName() + "_db");

function setupCollection(coll) {
    const docs = [];
    for (let i = 0; i < 200; ++i) {
        docs.push({_id: i, index: i});
    }
    assert.commandWorked(coll.insertMany(docs));
}

function assertCursorReleased(cursorId) {
    const releaseMemoryRes = db.runCommand({releaseMemory: [cursorId]});
    assert.commandWorked(releaseMemoryRes);
    assert.eq(releaseMemoryRes.cursorsReleased, [cursorId], releaseMemoryRes);
}

const coll = db[jsTestName()];
setupCollection(coll);

const cmdRes = coll.getDB().runCommand({
    find: coll.getName(),
    filter: {},
    sort: {index: 1},
    batchSize: 5,
    readConcern: {level: "snapshot"}
});
assert.commandWorked(cmdRes);
const cursorId = cmdRes.cursor.id;

assertCursorReleased(cursorId);
assert.commandWorked(db.runCommand({getMore: cursorId, collection: jsTestName(), batchSize: 1}));

coll.drop();

assertCursorReleased(cursorId);
assert.commandWorked(db.runCommand({getMore: cursorId, collection: jsTestName(), batchSize: 1}));

rst.stopSet();
