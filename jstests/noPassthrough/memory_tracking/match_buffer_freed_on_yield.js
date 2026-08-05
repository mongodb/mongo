/**
 * Check that classic $match frees its reusable BSON buffer when yielding.
 */
const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryFrameworkControl: "forceClassicEngine",
        featureFlagQueryMemoryTracking: true,
        featureFlagExpressionMemoryTracking: true,
    },
});
const db = conn.getDB(jsTestName());
const coll = db.test;
coll.drop();

assert.commandWorked(
    coll.insertMany(
        Array.from({length: 10}, (_, i) => ({_id: i, pad: "x".repeat(64 * 1024), target: i % 2})),
    ),
);

// $addFields makes docs non-trivially-convertible (forces slow path); $expr forces needWholeDocument.
const pipeline = [
    {$addFields: {computed: {$concat: ["$pad", "_suffix"]}}},
    {$match: {$expr: {$eq: ["$target", 1]}}},
];

const session = db.getMongo().startSession();
const sessionDb = session.getDatabase(db.getName());
const sessionId = session.getSessionId();

const cursorId = assert.commandWorked(
    sessionDb.runCommand({aggregate: coll.getName(), pipeline, cursor: {batchSize: 1}}),
).cursor.id;
assert.neq(cursorId, NumberLong(0));

assert.commandWorked(
    sessionDb.runCommand({
        getMore: cursorId,
        collection: coll.getName(),
        lsid: sessionId,
        batchSize: 1,
    }),
);

const idleDoc = db
    .getSiblingDB("admin")
    .aggregate([
        {$currentOp: {localOps: true, idleCursors: true}},
        {$match: {"lsid.id": sessionId.id, "type": "idleCursor"}},
    ])
    .toArray()[0];
assert(idleDoc, "expected one idle cursor");

assert.gt(idleDoc.peakTrackedMemBytes, 64 * 1024, tojson(idleDoc));
// inUseTrackedMemBytes is absent when 0 (buffer freed on detach).
assert(!idleDoc.hasOwnProperty("inUseTrackedMemBytes"), tojson(idleDoc));

session.endSession();
MongoRunner.stopMongod(conn);
