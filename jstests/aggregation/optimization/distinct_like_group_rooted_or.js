/**
 * Ensures that distinct queries run correctly with a rooted $or.
 */

// TODO SERVER-101473: This generates two fetch nodes, we should fix that.
const collName = jsTestName();
const coll = db[collName];

coll.drop();
assert.commandWorked(coll.insert({}));
assert.commandWorked(coll.createIndex({t: 1, m: 1}));
assert.commandWorked(db.runCommand({
    aggregate: jsTestName(),
    pipeline:
        [{"$match": {"$or": [{"t": {"$lte": null}}, {"t": 0}], "a": 0}}, {"$group": {"_id": "$m"}}],
    cursor: {}
}));
