/**
 * Tests recordIds show up when inserting into a collection with the 'recordIdsReplicated'
 * flag set even when inserting from within a transaction or when using the applyOps command.
 *
 * @tags: [
 *   featureFlagRecordIdsReplicated,
 * ]
 */
import {planHasStage} from "jstests/libs/analyze_plan.js";
import {
    validateShowRecordIdReplicatesAcrossNodes,
} from "jstests/libs/replicated_record_ids_utils.js";

const replSet = new ReplSetTest({nodes: 2});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondaries()[0];

const unRepRidlNs = 'unreplRecIdColl';
const replRidNs = 'replRecIdColl';
const dbName = 'test';

const primDB = primary.getDB(dbName);
const secDB = secondary.getDB(dbName);
primDB.runCommand({create: unRepRidlNs, recordIdsReplicated: false});
primDB.runCommand({create: replRidNs, recordIdsReplicated: true});

const session = primDB.getMongo().startSession();
const unReplRidColl = session.getDatabase(dbName)[unRepRidlNs];
const replRidColl = session.getDatabase(dbName)[replRidNs];

// Validates the most recent 'applyOps' oplog entry on the primary has the 'rid' field on operations
// performed on the collection with replicated record ids.
const validateMostRecentApplyOpsInOplogs = function() {
    const matchApplyOps = {
        '$match': {ns: 'admin.$cmd', 'o.applyOps.ns': replRidColl.getFullName()}
    };
    const sortForMostRecent = {'$sort': {txnNumber: -1}};
    const limit = {'$limit': 1};
    const unwind = {'$unwind': {path: '$o.applyOps'}};
    const groupByNamespaceAndRidField = {
        '$group': {
            _id: {
                ns: '$o.applyOps.ns',
                hasRid: {'$cond': {if: {'$gt': ['$o.applyOps.rid', null]}, then: true, else: false}}
            },
            count: {'$sum': 1}
        }
    };
    const getLatestApplyOpsForError = () => {
        return primDB.getSiblingDB('local')
            .oplog.rs.aggregate([matchApplyOps, sortForMostRecent, limit])
            .toArray();
    };

    // The aggregate returns the most recent 'applyOps' entry parsed into the following
    // format:
    //         [{_id: { ns: <>, hasRid: <> }, count: <>}, ....]
    // The '_id' maps to the aggregate count of ops in the applyOps that have 'ns' and the 'rid'
    // field set/unset.
    //
    // Use the aggregate to speed up checking whether the applyOps contain/omit the 'rid' field.

    // We expect there to only be 1 combination per for the replicated recordId namespace : {'ns':
    // replRidCollName, 'hasRid': true}. All non-replicated namespaces should take the form
    // of {'ns': <>, 'hasRid': false}.
    const applyOpsAggResult =
        primDB.getSiblingDB('local')
            .oplog.rs
            .aggregate(
                [matchApplyOps, sortForMostRecent, limit, unwind, groupByNamespaceAndRidField])
            .toArray();

    let containsReplNs = false;
    for (let aggRes of applyOpsAggResult) {
        if (aggRes._id.ns === replRidColl.getFullName()) {
            assert.eq(
                aggRes._id.hasRid,
                true,
                `Expected for all 'ops' for the replicated ns to have the rid field. Most recent applyOps entry ${
                    tojson(getLatestApplyOpsForError())}`);
            containsReplNs = true;
        } else {
            assert.neq(
                aggRes._id.hasRid,
                true,
                `Expected for all 'ops' for ns without replciate recordIds to omit the 'rid' field. Most recent applyOps entry ${
                    tojson(getLatestApplyOpsForError())}`);
        }
    }
    assert(
        containsReplNs,
        `Expected for aggregate to contain entries for the namespace with replicated recordIds. Got agg results: ${
            tojson(applyOpsAggResult)}. Most recent apply ops entry: ${
            tojson(getLatestApplyOpsForError())}`);
};

// On replication, secondaries apply oplog entries in parallel - a batch of oplog entries is
// distributed amongst several appliers, who apply the entries in parallel. Therefore, if we
// insert a single document at a time, it is likely that the replicated oplog batches will have
// just a single oplog entry each time, and therefore the secondary will basically be processing
// oplog entries in the same order that they appear on the primary. If processed in the same order,
// it is likely that the secondaries will generate the same recordIds as the primary, even
// with recordIdsReplicated:false.
//
// Therefore to ensure that recordIdsReplicated:true actually works we need to make sure that
// the appliers process oplog entries in parallel, and this is done by having a full batch of
// entries for the appliers to process. We can achieve this by performing an insertMany.
const docs = [];
for (let i = 0; i < 100; i++) {
    docs.push({a: i});
}

jsTestLog("Testing that within a transaction the recordIds are preserved on insert.");
session.startTransaction();
assert.commandWorked(replRidColl.insertMany(docs));
session.commitTransaction();
validateShowRecordIdReplicatesAcrossNodes(replSet.nodes, dbName, replRidNs);
validateMostRecentApplyOpsInOplogs();

jsTestLog("Testing that within a transaction the recordIds are preserved on delete.");
session.startTransaction();
assert.commandWorked(replRidColl.remove({}));
session.commitTransaction();
validateMostRecentApplyOpsInOplogs();

jsTestLog("Test writing to multiple collections.");
// This time, write to a collection with recordIdsReplicated:false and recordIdsReplicated:true
// within the same txn.
session.startTransaction();
assert.commandWorked(unReplRidColl.insertMany(docs));
assert.commandWorked(replRidColl.insertMany(docs));
assert.commandWorked(unReplRidColl.insertMany(docs));
assert.commandWorked(replRidColl.insertMany(docs));
session.commitTransaction();
validateShowRecordIdReplicatesAcrossNodes(replSet.nodes, dbName, replRidNs);
validateMostRecentApplyOpsInOplogs();

jsTestLog("Test deletes on multiple collections.");
session.startTransaction();
assert.commandWorked(replRidColl.remove({}));
assert.commandWorked(unReplRidColl.remove({}));
session.commitTransaction();
validateMostRecentApplyOpsInOplogs();

jsTestLog("Testing oplog consistency between primary and secondary after transactions");
replSet.checkOplogs();

jsTestLog("Testing that within an applyOps command the recordIds are preserved.");
let ops = [];
for (let i = 0; i < 20; i++) {
    ops.push({op: "i", ns: unReplRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
    ops.push({op: "i", ns: replRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
    if (i % 4) {
        ops.push({op: "d", ns: unReplRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
        ops.push({op: "d", ns: replRidColl.getFullName(), o: {_id: i}, o2: {_id: i}});
    }
}
assert.commandWorked(primDB.runCommand({applyOps: ops}));
validateShowRecordIdReplicatesAcrossNodes(replSet.nodes, dbName, replRidNs);
validateMostRecentApplyOpsInOplogs();

jsTestLog("Testing BATCHED_DELETE preserves recordIds outside of transaction");
// First, confirm that the remove will result in a BATCHED_DELETE.
const explainedBatchRemove = replRidColl.explain().remove({});
assert(planHasStage(primDB, explainedBatchRemove, "BATCHED_DELETE"));
assert.commandWorked(replRidColl.remove({}));
validateMostRecentApplyOpsInOplogs();

assert.commandWorked(unReplRidColl.remove({}));

jsTestLog("Testing that providing recordIds to applyOps keeps those recordIds.");
ops = [];
const numIters = 20;
let docsRemovedPerColl = 0;
for (let i = 0; i < numIters; i++) {
    ops.push({
        op: "i",
        ns: unReplRidColl.getFullName(),
        o: {_id: i},
        o2: {_id: i},
    });
    ops.push({
        op: "i",
        ns: replRidColl.getFullName(),
        o: {_id: i},
        o2: {_id: i},
        rid: NumberLong(2000 + i)
    });

    if (i % 4 == 0) {
        ops.push({
            op: "d",
            ns: unReplRidColl.getFullName(),
            o: {_id: i},
            o2: {_id: i},
        });
        ops.push({
            op: "d",
            ns: replRidColl.getFullName(),
            o: {_id: i},
            o2: {_id: i},
            rid: NumberLong(2000 + i)
        });
        docsRemovedPerColl++;
    }
}

assert.commandWorked(primDB.runCommand({applyOps: ops}));
assert.eq(replRidColl.find().count(), numIters - docsRemovedPerColl);
validateShowRecordIdReplicatesAcrossNodes(replSet.nodes, dbName, replRidNs);
validateMostRecentApplyOpsInOplogs();

/**
// TODO SERVER-78350: Enable testing incorrect rids.
jsTestLog("Testing that providing non-existent recordIds to applyOps are no-ops.");
assert.commandWorked(replRidColl.remove({}));

ops = [];
docsRemovedPerColl = 0;
for (let i = 0; i < numIters; i++) {
    ops.push({
        op: "i",
        ns: replRidColl.getFullName(),
        o: {_id: i, a: 1},
        o2: {_id: i},
        rid: NumberLong(i),
    });

    ops.push({
        op: "d",
        ns: replRidColl.getFullName(),
        o: {_id: i},
        o2: {_id: i},
        rid: NumberLong(2000 + i)
    });
}

assert.commandWorked(primDB.runCommand({applyOps: ops}));
assert.eq(replRidColl.find().count(), numIters - docsRemovedPerColl);
*/
replSet.stopSet();
