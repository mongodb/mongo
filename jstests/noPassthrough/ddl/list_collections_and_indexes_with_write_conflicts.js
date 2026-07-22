/**
 * Validates that listIndexes and listCollections retry internally on write conflicts and do not
 * bubble the error up to the user.
 *
 * @tags: [
 *   requires_wiredtiger,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const kIterations = 100;
const kFailPointProbability = 0.75;

function runForIterations(testFn) {
    for (let i = 0; i < kIterations; ++i) {
        testFn();
    }
}

const conn = MongoRunner.runMongod();
const dbName = jsTestName();
const collName = "coll";
const coll2Name = "coll2";
const db = conn.getDB(dbName);
const coll = db[collName];

coll.drop();
assert.commandWorked(db.createCollection(collName));
assert.commandWorked(db.createCollection(coll2Name));

assert.commandWorked(coll.createIndex({x: 1}));
assert.commandWorked(coll.createIndex({x: -1, y: 1}));

let readFP = configureFailPoint(
    conn,
    "WTWriteConflictExceptionForReads",
    {} /* data */,
    {activationProbability: kFailPointProbability},
);

jsTest.log.info("Running listIndexes with high probability of write conflicts");
function testListIndexes() {
    let res = assert.commandWorked(db.runCommand({listIndexes: collName}));
    assert.eq(res.cursor.firstBatch.length, 3);
}
runForIterations(testListIndexes);

jsTest.log.info("Running listCollections with high probability of write conflicts");
function testListCollections() {
    let res = assert.commandWorked(db.runCommand({listCollections: 1}));
    assert.eq(res.cursor.firstBatch.length, 2);
}
runForIterations(testListCollections);

jsTest.log.info(
    "Running listCollections with cursor response and high probability of write conflicts",
);
function testListCollectionsWithCursorResponse() {
    let res = assert.commandWorked(db.runCommand({listCollections: 1, cursor: {batchSize: 1}}));
    assert.eq(res.cursor.firstBatch.length, 1);
    // Exhaust the cursor
    let cursorId = res.cursor.id;
    res = assert.commandWorked(
        db.runCommand({getMore: cursorId, collection: "$cmd.listCollections"}),
    );
    assert.eq(res.cursor.nextBatch.length, 1);
}

runForIterations(testListCollectionsWithCursorResponse);

readFP.off();

db.dropDatabase();
MongoRunner.stopMongod(conn);
