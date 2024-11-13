/**
 * @tags: [
 *   requires_capped,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 * ]
 */

let t = db[jsTestName()];
t.drop();

db.createCollection(jsTestName(), {capped: true, size: 1024});
let v = t.validate();
assert(v.valid, "A : " + tojson(v));  // SERVER-485

t.save({x: 1});
assert(t.validate().valid, "B");
