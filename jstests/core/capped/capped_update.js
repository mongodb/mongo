/**
 * Tests various update scenarios on capped collections:
 *  -- SERVER-58865: Allow modifications that change capped document sizes.
 *
 * @tags: [
 *   requires_capped,
 *   uses_testing_only_commands,
 *   # godinsert and can't run under replication
 *   assumes_standalone_mongod,
 *   # capped collections cannot be sharded
 *   assumes_unsharded_collection,
 *   no_selinux,
 * ]
 */

const localDB = db.getSiblingDB("local");
const t = localDB.capped_update;
t.drop();

assert.commandWorked(localDB.createCollection(t.getName(), {capped: true, size: 1024}));

let docs = [];
for (let j = 1; j <= 10; j++) {
    docs.push({_id: j, s: "Hello, World!"});
}
assert.commandWorked(t.insert(docs));

assert.commandWorked(t.update({_id: 3}, {s: "Hello, Mongo!"}));  // Mongo is same length as World
assert.commandWorked(t.update({_id: 3}, {$set: {s: "Hello!"}}));
assert.commandWorked(t.update({_id: 10}, {}));
assert.commandWorked(t.update({_id: 10}, {s: "Hello, World!!!"}));
