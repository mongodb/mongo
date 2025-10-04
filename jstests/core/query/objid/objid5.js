/**
 * @tags: [
 *   # The machine OID value is global and cannot be modified by multiple clients simultaneously.
 *   multi_clients_incompatible,
 ]
 */
let t = db.objid5;
t.drop();

t.save({_id: 5.5});
assert.eq(18, Object.bsonsize(t.findOne()), "A");

let x = db.runCommand({features: 1});
let y = db.runCommand({features: 1, oidReset: 1});

if (!x.ok) print("x: " + tojson(x));

assert(x.oidMachine, "B1");
assert.neq(x.oidMachine, y.oidMachine, "B2");
assert.eq(x.oidMachine, y.oidMachineOld, "B3");

assert.eq(18, Object.bsonsize({_id: 7.7}), "C1");
assert.eq(0, Object.bsonsize(null), "C2");
