/**
 * Test that verifies that write concern error is logged as part of slow query logging.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {stopReplicationOnSecondaries, restartReplicationOnSecondaries} from "jstests/libs/write_concern_util.js";
import {assertHasWCE} from "jstests/libs/write_concern_all_commands.js";

const rst = new ReplSetTest({nodes: 3});
rst.startSet();
rst.initiate();

const dbName = jsTestName();
const collName = "testcoll";
const primary = rst.getPrimary();
const session = primary.getDB(dbName).getMongo().startSession({causalConsistency: false});
const primaryDB = session.getDatabase(dbName);

stopReplicationOnSecondaries(rst);
const coll = db[collName];

const res = primaryDB.runCommand({
    insert: collName,
    documents: [{}],
    writeConcern: {w: "majority"},
    maxTimeMS: 1000,
});
assertHasWCE(res);

// writeConcernError should be present in the slow query log.
const predicate = new RegExp(`Slow query.*writeConcernError"`);
assert(checkLog.checkContainsOnce(primary, predicate), "Could not find log containing " + predicate);

restartReplicationOnSecondaries(rst);
rst.stopSet();
