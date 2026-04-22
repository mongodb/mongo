/**
 * Regression test: a change stream opened on a specific collection must not tassert when
 * featureFlagPathArrayness is enabled. Previously, the main-collection arrayness registration
 * in aggregation_execution_state.cpp compared `mainColl->ns()` (the oplog, for change streams)
 * to `mainNss` (the user-facing nss), which always mismatched.
 *
 * @tags: [
 *   requires_replication,
 *   requires_majority_read_concern,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {featureFlagPathArrayness: true}},
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const testDb = primary.getDB(jsTestName());
const coll = testDb.watched;

assert.commandWorked(coll.insert({_id: 0}));

const cs = coll.watch();
assert.commandWorked(coll.insert({_id: 1}));
assert.soon(() => cs.hasNext(), "change stream produced no event");
const evt = cs.next();
assert.eq(evt.operationType, "insert");
assert.eq(evt.fullDocument._id, 1);
cs.close();

// Whole-db change stream exercises the same OplogAggCatalogState path.
const wholeDbCs = testDb.watch();
assert.commandWorked(coll.insert({_id: 2}));
assert.soon(() => wholeDbCs.hasNext(), "whole-db change stream produced no event");
const wholeDbEvt = wholeDbCs.next();
assert.eq(wholeDbEvt.operationType, "insert");
assert.eq(wholeDbEvt.fullDocument._id, 2);
wholeDbCs.close();

rst.stopSet();
