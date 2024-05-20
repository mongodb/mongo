// Tests that $group aggregation works with group key of type ObjectId.
// @tags: [
//   # Some in memory variants will error because this test uses too much memory. As such, we do not
//   # run this test on in-memory variants.
//   requires_persistence,
// ]
// TODO SERVER-90234: Use jsTestName() instead of a string.
const collName = "group_by_objectid";
const coll = db[collName];
coll.drop();

const bigStr = Array(100 * 1000).toString();  // ~ 100KB of ','
const bigStr2 = bigStr + "2";
const nDocs = 1000;
const nGroups = 10;

let objectIds = [];
for (let i = 0; i < nGroups; i++) {
    objectIds.push(new ObjectId());
}

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= nDocs; i++) {
    bulk.insert({b: objectIds[i % nGroups], bigStr: bigStr, b2: bigStr2, c: i});
}
assert.commandWorked(bulk.execute());

const pipeline = [
    {
        $sort: {
            "c": NumberInt(-1),
        }
    },
    {$group: {"_id": "$b", "doc": {"$first": "$$ROOT"}}},
];

assert.commandWorked(db.runCommand({aggregate: collName, pipeline: pipeline, cursor: {}}));
