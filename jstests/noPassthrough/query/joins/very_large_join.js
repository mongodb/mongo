/**
 * Tests that the join optimizer can handle a number of joins larger then the join graph can store.
 * @tags: [
 *   requires_fcv_83,
 * ]
 */

const kMaxNodesInJoin = 64;
const numberOfJoins = kMaxNodesInJoin + 10;

const docs = [
    {a: 1, b: 1},
    {a: 1, b: 2},
];

const config = {
    setParameter: {
        internalEnableJoinOptimization: true,
        internalJoinReorderMode: "random",
        internalRandomJoinOrderSeed: 42,
    },
};

const conn = MongoRunner.runMongod(config);

const db = conn.getDB(jsTestName());

db.coll.drop();
assert.commandWorked(db.coll.insertMany(docs));

const pipeline = [];
let prevCollName = null;
for (let i = 0; i < numberOfJoins; ++i) {
    const from = `coll${i}`;
    const coll = db[from];
    coll.drop();
    assert.commandWorked(coll.insertMany(docs));

    const localField = prevCollName == null ? "a" : `${prevCollName}.a`;
    const foreignField = "b";

    pipeline.push({"$lookup": {from, localField, foreignField, as: from}});
    pipeline.push({"$unwind": {path: `$${from}`}});

    prevCollName = from;
}

const result = db.coll.aggregate(pipeline).toArray();
assert.eq(result.length, 2);

MongoRunner.stopMongod(conn);
