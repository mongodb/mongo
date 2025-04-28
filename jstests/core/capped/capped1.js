/**
 * @tags: [
 *   requires_capped,
 *   # capped collections cannot be sharded
 *   assumes_unsharded_collection,
 * ]
 */

let t = db[jsTestName()];
assert(t.drop());

assert.commandWorked(db.createCollection(jsTestName(), {capped: true, size: 1024}));
let v = assert.commandWorked(t.validate());
assert(v.valid, "A : " + tojson(v));  // SERVER-485

assert.commandWorked(t.save({x: 1}));
assert(assert.commandWorked(t.validate()).valid, "B");
