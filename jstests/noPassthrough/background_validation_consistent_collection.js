/**
 * Tests that background validation obtains a collection instance consistent with the checkpoint
 * timestamp.
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {
        setParameter: {
            // Disable the checkpoint thread.
            syncdelay: 0,
            logComponentVerbosity: tojson({storage: {wt: {wtCheckpoint: 1}}})
        }
    }
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();

const dbName = "test";
const collName = jsTestName();

const db = primary.getDB(dbName);
const coll = db.getCollection(collName);

assert.commandWorked(db.createCollection(collName));

assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({_id: 1, a: 100}));
assert.commandWorked(coll.insert({_id: 2, a: 100}));

assert.commandWorked(db.adminCommand({fsync: 1}));

let res = assert.commandWorked(coll.validate({background: true}));
assert(res.valid);
assert.eq(2, res.nrecords);

assert.commandWorked(
    db.runCommand({collMod: collName, index: {keyPattern: {a: 1}, prepareUnique: true}}));

assert.commandWorked(coll.remove({_id: 2}));
assert.commandWorked(db.adminCommand({
    applyOps: [
        {
            op: 'c',
            ns: db.$cmd.getFullName(),
            o: {
                collMod: coll.getName(),
                index: {
                    keyPattern: {a: 1},
                    unique: true,
                },
            },
        },
    ]
}));

res = assert.commandWorked(coll.validate({background: true}));
assert(res.valid);
assert.eq(2, res.nrecords);

rst.stopSet();
