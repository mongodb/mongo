/**
 * Tests recordIds show up when inserting into a collection with the 'recordIdsReplicated'
 * flag set even when inserting from within a transaction or when using the applyOps command.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondaries()[0];

const unRepRidlNs = 'unreplRecIdColl';
const replRidNs = 'replRecIdColl';

const primDB = primary.getDB('test');
const secDB = secondary.getDB('test');
primDB.runCommand({create: unRepRidlNs, recordIdsReplicated: false});
primDB.runCommand({create: replRidNs, recordIdsReplicated: true});

const session = primDB.getMongo().startSession();
const unReplRidColl = session.getDatabase('test')[unRepRidlNs];
const replRidColl = session.getDatabase('test')[replRidNs];

const docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({a: i});
}

jsTestLog("Testing that within a transaction the recordIds are preserved on insert.");
session.startTransaction();
assert.commandWorked(replRidColl.insertMany(docs));
session.commitTransaction();

let primCursor = primDB[replRidNs].find().sort({_id: 1}).showRecordId();
let secCursor = secDB[replRidNs].find().sort({_id: 1}).showRecordId();
assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

assert.commandWorked(replRidColl.remove({}));

jsTestLog("Test writing to multiple collections.");
// This time, write to a collection with recordIdsReplicated:false and recordIdsReplicated:true
// within the same txn.
session.startTransaction();
assert.commandWorked(unReplRidColl.insertMany(docs));
assert.commandWorked(replRidColl.insertMany(docs));
assert.commandWorked(unReplRidColl.insertMany(docs));
assert.commandWorked(replRidColl.insertMany(docs));
session.commitTransaction();

primCursor = primDB[replRidNs].find().sort({_id: 1}).showRecordId();
secCursor = secDB[replRidNs].find().sort({_id: 1}).showRecordId();
assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

assert.commandWorked(replRidColl.remove({}));
assert.commandWorked(unReplRidColl.remove({}));

jsTestLog("Testing that within an applyOps command the recordIds are preserved.");
let ops = [];
for (let i = 0; i < 100; i++) {
    if (i % 4 == 0) {
        ops.push({op: "i", ns: unReplRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
    } else {
        ops.push({op: "i", ns: replRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
    }
}
assert.commandWorked(primDB.runCommand({applyOps: ops}));
primCursor = primDB[replRidNs].find().sort({_id: 1}).showRecordId();
secCursor = secDB[replRidNs].find().sort({_id: 1}).showRecordId();
assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

assert.commandWorked(replRidColl.remove({}));
assert.commandWorked(unReplRidColl.remove({}));

jsTestLog("Testing that providing recordIds to applyOps keeps those recordIds.");
ops = [];
for (let i = 0; i < 100; i++) {
    if (i % 4 == 0) {
        ops.push({
            op: "i",
            ns: unReplRidColl.getFullName(),
            o: {_id: i},
            o2: {_id: i},
        });
    } else {
        ops.push({
            op: "i",
            ns: replRidColl.getFullName(),
            o: {_id: i},
            o2: {_id: i},
            rid: NumberLong(2000 + i)
        });
    }
}

assert.commandWorked(primDB.runCommand({applyOps: ops}));
primCursor = primDB[replRidNs].find().sort({_id: 1}).showRecordId();
secCursor = secDB[replRidNs].find().sort({_id: 1}).showRecordId();
assert.eq({docsWithDifferentContents: [], docsMissingOnFirst: [], docsMissingOnSecond: []},
          DataConsistencyChecker.getDiff(primCursor, secCursor));

replSet.stopSet();
