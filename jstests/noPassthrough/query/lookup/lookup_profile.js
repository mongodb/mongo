// Tests that lookups on local capped collections acquire a snapshot on the capped collection
// correctly. Tests the scenario fixed by SERVER-91203 no longer causes a crash.
//  @tags: [
//      # TODO SERVER-121515: Remove after updating test to account for profiling level > 0 not being supported in disagg.
//      requires_profiling,
//  ]

import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({nodes: {n0: {profile: "0"}}});
rst.startSet();
rst.initiate();

const dbName = "test";
const collName = "foo";

let testDB = rst.getPrimary().getDB(dbName);
let testColl = testDB.getCollection(collName);

testColl.insert({a: 1});

testDB.setProfilingLevel(2);

const pipeline = [{$lookup: {from: "system.profile", localField: "key", foreignField: "key", as: "results"}}];
testColl.aggregate(pipeline).toArray();

rst.stopSet();
